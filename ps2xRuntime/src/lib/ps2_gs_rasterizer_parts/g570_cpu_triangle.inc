// ===== G570: exact CPU replay kernel for the combat 0x139 untextured triangle class =====
//
// The G570 combat census found that the jump/dash stalls are pending-graph flushes of FBP 0x139.
// The dominant entries are untextured, blended triangle fans with TEST=0x5000a and a read-only
// PSMZ24 mirror at ZBP 0xd0.  The generic triangle loop calls g403DisplayZRead() and writePixel()
// for every accepted fragment.  Ceiling probes show those two calls own nearly the whole replay.
//
// This helper keeps the established float coverage/interpolation loop and changes only its two leaf
// operations:
//   * read the already-authoritative G403/G411 linear depth mirror through a row pointer;
//   * lower the exact CT32 writePixel semantics into a per-triangle schedule and per-row swizzle.
//
// Research alternatives / oracles:
//   DC2_G570_CPU_139=1          enable the candidate
//   DC2_G570_VERIFY_CPU_139=1   run the candidate calculation, let generic writePixel commit, and
//                               compare the committed word plus the direct depth read
//   DC2_G570_LINEAR_139=1       deswizzle each replay lane once and use linear color rows
//   DC2_G570_VERIFY_LINEAR_139=1 run both linear and generic color paths, compare the whole band,
//                               and keep the generic output authoritative
//   DC2_G570_DELTA_139=1        accumulate the measured +/-1 shadow mask in a signed surface
//   DC2_G570_VERIFY_DELTA_139=1 compare that final reduction with the ordered generic replay
//   DC2_G570_CPU_STAT=1         coverage census without the per-pixel oracle
// G571 shipping path / rollback:
//   exact whole-batch CPU fallback + exact GPU compute + conservative tile culling are default-ON
//   DC2_G570_NO_GPU_139=1 / DC2_G570_NO_BATCH_139=1 / DC2_G570_NO_TILE_CULL_139=1
// Research rollback:
//   DC2_G570_NO_CPU_139=1
//   DC2_G570_NO_LINEAR_139=1

// The candidate deliberately fails closed whenever a diagnostic which observes the generic pixel
// path is armed.  Its caller additionally admits only the exact state listed above.

namespace
{

inline bool g570Cpu139VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_CPU_139");
    return on;
}

inline bool g570Cpu139LinearVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_LINEAR_139");
    return on;
}

inline bool g570Cpu139LinearOn()
{
    static const bool on =
        !envFlagEnabled("DC2_G570_NO_LINEAR_139") &&
        (envFlagEnabled("DC2_G570_LINEAR_139") || g570Cpu139LinearVerifyOn());
    return on;
}

inline bool g570Cpu139DeltaVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_DELTA_139");
    return on;
}

inline bool g570Cpu139DeltaOn()
{
    static const bool on =
        !envFlagEnabled("DC2_G570_NO_DELTA_139") &&
        (envFlagEnabled("DC2_G570_DELTA_139") || g570Cpu139DeltaVerifyOn());
    return on;
}

inline bool g570Cpu139StatOn()
{
    static const bool on = envFlagEnabled("DC2_G570_CPU_STAT") || g570Cpu139VerifyOn();
    return on;
}

inline bool g570Cpu139On()
{
    // Bring-up default: OFF.  Promotion flips this to default-on after the same-run oracle and
    // paired route gates pass; the explicit arm remains accepted for reproducibility.
    static const bool on =
        !envFlagEnabled("DC2_G570_NO_CPU_139") &&
        (envFlagEnabled("DC2_G570_CPU_139") || g570Cpu139VerifyOn() ||
         g570Cpu139LinearOn() || g570Cpu139DeltaOn());
    return on;
}

inline bool g570Cpu139Quiet()
{
    static const bool quiet =
        g530Quiet() &&
        !envFlagEnabled("DC2_G570_VERIFY_139") &&
        !envFlagEnabled("DC2_G570_PRIVATE_139") &&
        !envFlagEnabled("DC2_G282_CENSUS") &&
        !envFlagEnabled("DC2_G232_RTTDUMP") &&
        !envFlagEnabled("DC2_G48_DARK") &&
        !envFlagEnabled("DC2_G51_SKIPLOWA") &&
        !envFlagEnabled("DC2_G141_SKIP_WRITE") &&
        !envFlagEnabled("DC2_G141_SKIP_SAMPLE") &&
        !envFlagEnabled("DC2_G141_SAMPLE_VERIFY") &&
        !envFlagEnabled("DC2_G44_FACE_Z") &&
        !envFlagEnabled("DC2_G43_FACE_UV") &&
        !envFlagEnabled("DC2_G82_ROCK");
    return quiet;
}

static bool g570Cpu139BatchOn()
{
    return g570Cpu139On() && g570Cpu139Quiet();
}

constexpr int kG570DepthTileShift = 4;
constexpr int kG570DepthTileSize = 1 << kG570DepthTileShift;
constexpr int kG570DepthTilesX =
    (static_cast<int>(kG403DisplayZW) + kG570DepthTileSize - 1) / kG570DepthTileSize;
constexpr int kG570DepthTilesY =
    (static_cast<int>(kG403DisplayZH) + kG570DepthTileSize - 1) / kG570DepthTileSize;
std::array<uint32_t, kG570DepthTilesX * kG570DepthTilesY> g_g570DepthTileMin{};
std::array<uint32_t, kG570DepthTilesX * kG570DepthTilesY> g_g570DepthTileMax{};
bool g_g570DepthTilesReady = false;

static void g570Cpu139PrepareDepthTiles()
{
    const std::vector<uint32_t> &depth = g403DisplayZBuf();
    if (depth.size() < static_cast<size_t>(kG403DisplayZW) * kG403DisplayZH)
    {
        g_g570DepthTilesReady = false;
        return;
    }
    for (int ty = 0; ty < kG570DepthTilesY; ++ty)
    {
        for (int tx = 0; tx < kG570DepthTilesX; ++tx)
        {
            uint32_t lo = 0xffffffffu, hi = 0u;
            const int y1 = std::min(static_cast<int>(kG403DisplayZH),
                                    (ty + 1) * kG570DepthTileSize);
            const int x1 = std::min(static_cast<int>(kG403DisplayZW),
                                    (tx + 1) * kG570DepthTileSize);
            for (int y = ty * kG570DepthTileSize; y < y1; ++y)
            {
                const uint32_t *row = depth.data() +
                    static_cast<size_t>(y) * kG403DisplayZW;
                for (int x = tx * kG570DepthTileSize; x < x1; ++x)
                {
                    const uint32_t z = row[x];
                    lo = std::min(lo, z);
                    hi = std::max(hi, z);
                }
            }
            const size_t i = static_cast<size_t>(ty) * kG570DepthTilesX + tx;
            g_g570DepthTileMin[i] = lo;
            g_g570DepthTileMax[i] = hi;
        }
    }
    g_g570DepthTilesReady = true;
}

static int g570Cpu139DepthClass(int x0, int y0, int x1, int y1,
                                 float z0, float z1, float z2)
{
    if (!g_g570DepthTilesReady || x1 < x0 || y1 < y0)
        return 0;
    const int tx0 = std::max(0, x0 >> kG570DepthTileShift);
    const int ty0 = std::max(0, y0 >> kG570DepthTileShift);
    const int tx1 = std::min(kG570DepthTilesX - 1, x1 >> kG570DepthTileShift);
    const int ty1 = std::min(kG570DepthTilesY - 1, y1 >> kG570DepthTileShift);
    uint32_t depthLo = 0xffffffffu, depthHi = 0u;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
        {
            const size_t i = static_cast<size_t>(ty) * kG570DepthTilesX + tx;
            depthLo = std::min(depthLo, g_g570DepthTileMin[i]);
            depthHi = std::max(depthHi, g_g570DepthTileMax[i]);
        }
    constexpr float margin = 64.0f;
    const float triLo = std::max(0.0f, std::min({z0, z1, z2}) - margin);
    const float triHi = std::min(16777215.0f, std::max({z0, z1, z2}) + margin);
    if (triLo >= static_cast<float>(depthHi))
        return 1;
    if (triHi < static_cast<float>(depthLo))
        return -1;
    return 0;
}

// G570 batch kernel.  The generic replay calls drawTriangle once per row lane, repeating the
// triangle's large state/setup front-end and performing one swizzled VRAM RMW per shadow hit.  This
// path validates the whole captured list, prepares invariant triangle equations once, then replays
// the same ordered fragments into a lane-local lazy color cache.  Each distinct color pixel is read
// and written once; fixed-edge coverage, float interpolation, GEQUAL depth, saturation order, and
// source alpha remain identical.
inline bool g570Batch139VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_BATCH_139");
    return on;
}

inline bool g570Batch139GpuVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_GPU_139");
    return on;
}

inline bool g570Batch139GpuOn()
{
    // G571 promotion: exact GPU authority is the shipping path.  The explicit arm remains
    // accepted for old scripts; NO_GPU restores the exact CPU batch below.
    static const bool on = !envFlagEnabled("DC2_G570_NO_GPU_139");
    return on;
}

// G571: the GPU shader only needs to visit the exact fixed-edge X interval for each row.  Keep
// this default-on inside the guarded GPU candidate; the rollback restores G570's full-bbox spans.
inline bool g570Batch139TightGpuSpansOn()
{
    static const bool on = !envFlagEnabled("DC2_G570_NO_TIGHT_SPAN_139") &&
                           envFlagEnabled("DC2_G570_TIGHT_SPAN_139");
    return on;
}

// G571: reject 16x16 bins that lie wholly outside at least one exact triangle half-plane.  This
// is conservative (a bin is retained on any boundary ambiguity) and avoids per-pixel iteration
// over bbox-only triangle references.  Default-on inside G570; rollback restores bbox binning.
inline bool g570Batch139TileCullOn()
{
    static const bool on = !envFlagEnabled("DC2_G570_NO_TILE_CULL_139");
    return on;
}

inline bool g570Batch139On()
{
    // G571 promotion: the exact whole-batch kernel is also the default fail-closed CPU fallback
    // when the compute backend is unavailable or explicitly rolled back.
    static const bool on = !envFlagEnabled("DC2_G570_NO_BATCH_139");
    return on;
}

struct G570Batch139Tri
{
    int minX = 0, maxX = -1, minY = 0, maxY = -1;
    int64_t fx[3]{};
    int64_t fy[3]{};
    int64_t step[3]{};
    int64_t rowStep[3]{};
    int64_t edgeBase[3]{};
    uint8_t topLeftMask = 0u;
    int sign = 1;
    int8_t delta = 0;
    uint32_t spanOffset = 0u;
    int ofx = 0, ofy = 0;
    float rawX[3]{};
    float rawY[3]{};
    float a0 = 0.0f, b0 = 0.0f, a1 = 0.0f, b1 = 0.0f;
    float fx2 = 0.0f, fy2 = 0.0f;
    float winding = 1.0f, invAbsDenom = 0.0f;
    float z0 = 0.0f, z1 = 0.0f, z2 = 0.0f;
};

std::vector<G570Batch139Tri> g_g570Batch139Tris;
std::vector<uint32_t> g_g570Batch139Spans;
std::vector<uint32_t> g_g570Batch139GpuTris;
std::vector<uint32_t> g_g570Batch139GpuSpans;
std::vector<uint32_t> g_g570Batch139GpuTileOffsets;
std::vector<uint32_t> g_g570Batch139GpuTileTris;
std::vector<uint32_t> g_g570Batch139GpuDebug;
std::vector<uint32_t> g_g570Batch139CpuDebug;
int g_g570Batch139RejectReason = 1;

static int g570Batch139LastReject()
{
    return g_g570Batch139RejectReason;
}

static void g570Batch139ResetCpuDebug()
{
    g_g570Batch139CpuDebug.assign(
        static_cast<size_t>(kG403DisplayZW) * kG403DisplayZH, 0u);
}

static uint32_t g570Batch139GpuDebugAt(size_t index)
{
    return index < g_g570Batch139GpuDebug.size() ? g_g570Batch139GpuDebug[index] : 0u;
}

static uint32_t g570Batch139CpuDebugAt(size_t index)
{
    return index < g_g570Batch139CpuDebug.size() ? g_g570Batch139CpuDebug[index] : 0u;
}

static uint32_t g570Batch139PackedCoverageHash(int x, int y)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(kG403DisplayZW) ||
        y >= static_cast<int>(kG403DisplayZH) ||
        g_g570Batch139GpuTileOffsets.empty())
        return 0u;
    const auto loadI64 = [](const std::vector<uint32_t> &words, size_t at) {
        const uint64_t bits = static_cast<uint64_t>(words[at]) |
                              (static_cast<uint64_t>(words[at + 1u]) << 32u);
        int64_t value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const size_t tile = static_cast<size_t>(y >> 4) * 32u +
                        static_cast<size_t>(x >> 4);
    const uint32_t first = g_g570Batch139GpuTileOffsets[tile];
    const uint32_t last = g_g570Batch139GpuTileOffsets[tile + 1u];
    uint32_t hash = 0u;
    std::vector<uint8_t> packedCovered(g_g570Batch139Tris.size(), 0u);
    for (uint32_t ref = first; ref < last; ++ref)
    {
        const uint32_t tri = g_g570Batch139GpuTileTris[ref];
        const size_t b = static_cast<size_t>(tri) * 23u;
        const int minY = static_cast<int>(g_g570Batch139GpuTris[b + 2u]);
        const int maxY = static_cast<int>(g_g570Batch139GpuTris[b + 3u]);
        if (y < minY || y > maxY)
            continue;
        const size_t spanBase =
            (static_cast<size_t>(g_g570Batch139GpuTris[b + 4u]) +
             static_cast<size_t>(y - minY)) * 7u;
        const uint32_t packedSpan = g_g570Batch139GpuSpans[spanBase];
        const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
        const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
        if (x < xStart || x > xEnd)
            continue;
        const int64_t dx = static_cast<int64_t>(x - xStart);
        const int64_t e0 = loadI64(g_g570Batch139GpuSpans, spanBase + 1u) +
                           loadI64(g_g570Batch139GpuTris, b + 5u) * dx;
        const int64_t e1 = loadI64(g_g570Batch139GpuSpans, spanBase + 3u) +
                           loadI64(g_g570Batch139GpuTris, b + 7u) * dx;
        const int64_t e2 = loadI64(g_g570Batch139GpuSpans, spanBase + 5u) +
                           loadI64(g_g570Batch139GpuTris, b + 9u) * dx;
        const uint32_t mode = g_g570Batch139GpuTris[b + 11u];
        if (e0 < 0 || (e0 == 0 && (mode & 1u) == 0u) ||
            e1 < 0 || (e1 == 0 && (mode & 2u) == 0u) ||
            e2 < 0 || (e2 == 0 && (mode & 4u) == 0u))
            continue;
        packedCovered[tri] = 1u;
        hash = (hash * 251u + tri + 1u) & 0xffffu;
    }

    uint32_t directHash = 0u;
    size_t firstDiff = g_g570Batch139Tris.size();
    bool firstDiffDirect = false;
    uint32_t directCount = 0u, packedCount = 0u;
    for (size_t triIndex = 0; triIndex < g_g570Batch139Tris.size(); ++triIndex)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[triIndex];
        bool covered = false;
        if (p.maxX >= p.minX && y >= p.minY && y <= p.maxY)
        {
            const uint32_t packedSpan = g_g570Batch139Spans[
                static_cast<size_t>(p.spanOffset) + static_cast<size_t>(y - p.minY)];
            const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
            const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
            if (x >= xStart && x <= xEnd)
            {
                const int64_t sx = static_cast<int64_t>(x) * 16;
                const int64_t sy = static_cast<int64_t>(y) * 16;
                const int64_t edge0 = ((p.fx[2] - p.fx[1]) * (sy - p.fy[1]) -
                                       (p.fy[2] - p.fy[1]) * (sx - p.fx[1])) * p.sign;
                const int64_t edge1 = ((p.fx[0] - p.fx[2]) * (sy - p.fy[2]) -
                                       (p.fy[0] - p.fy[2]) * (sx - p.fx[2])) * p.sign;
                const int64_t edge2 = ((p.fx[1] - p.fx[0]) * (sy - p.fy[0]) -
                                       (p.fy[1] - p.fy[0]) * (sx - p.fx[0])) * p.sign;
                covered = !(edge0 < 0 || (edge0 == 0 && (p.topLeftMask & 1u) == 0u) ||
                            edge1 < 0 || (edge1 == 0 && (p.topLeftMask & 2u) == 0u) ||
                            edge2 < 0 || (edge2 == 0 && (p.topLeftMask & 4u) == 0u));
            }
        }
        if (covered)
        {
            directHash = (directHash * 251u + static_cast<uint32_t>(triIndex) + 1u) & 0xffffu;
            ++directCount;
        }
        packedCount += packedCovered[triIndex] != 0u ? 1u : 0u;
        if (covered != (packedCovered[triIndex] != 0u) &&
            firstDiff == g_g570Batch139Tris.size())
        {
            firstDiff = triIndex;
            firstDiffDirect = covered;
        }
    }
    static std::atomic<uint32_t> s_coverDiffLogs{0u};
    const uint32_t logIndex = s_coverDiffLogs.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex < 8u)
    {
        std::fprintf(stderr,
            "[G570:coverdiff] k=%u xy=(%d,%d) packed=%04x/%u direct=%04x/%u "
            "cpu=%04x firstDiff=%zu tileRefs=%u\n",
            logIndex, x, y, hash, packedCount, directHash, directCount,
            g570Batch139CpuDebugAt(static_cast<size_t>(y) * kG403DisplayZW + x) & 0xffffu,
            firstDiff, last - first);
        if (firstDiff < g_g570Batch139Tris.size())
        {
            const G570Batch139Tri &p = g_g570Batch139Tris[firstDiff];
            uint32_t span = 0u;
            if (y >= p.minY && y <= p.maxY)
                span = g_g570Batch139Spans[
                    static_cast<size_t>(p.spanOffset) + static_cast<size_t>(y - p.minY)];
            std::fprintf(stderr,
                "[G570:covertri] tri=%zu direct=%u packed=%u bbox=(%d,%d)-(%d,%d) "
                "span=(%d,%d) tile=(%d,%d)\n",
                firstDiff, firstDiffDirect ? 1u : 0u, packedCovered[firstDiff],
                p.minX, p.minY, p.maxX, p.maxY,
                static_cast<int16_t>(span & 0xffffu), static_cast<int16_t>(span >> 16u),
                x >> 4, y >> 4);
        }
    }
    return hash;
}

inline uint32_t g570Batch139FloatBits(float v)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline void g570Batch139PadGpuWords(std::vector<uint32_t> &words)
{
    constexpr size_t width = 1024u;
    if (!words.empty())
        words.resize(((words.size() + width - 1u) / width) * width, 0u);
}

inline bool g570Batch139TileMayCover(const G570Batch139Tri &p, int tx, int ty)
{
    constexpr int tileSize = 16;
    const int x0 = tx * tileSize;
    const int y0 = ty * tileSize;
    const int64_t dx = static_cast<int64_t>(x0 - p.minX);
    const int64_t dy = static_cast<int64_t>(y0 - p.minY);
    for (int edge = 0; edge < 3; ++edge)
    {
        int64_t maxEdge = p.edgeBase[edge] + p.step[edge] * dx + p.rowStep[edge] * dy;
        if (p.step[edge] > 0)
            maxEdge += p.step[edge] * (tileSize - 1);
        if (p.rowStep[edge] > 0)
            maxEdge += p.rowStep[edge] * (tileSize - 1);
        const int64_t threshold = (p.topLeftMask & (1u << edge)) != 0u ? 0 : 1;
        if (maxEdge < threshold)
            return false;
    }
    return true;
}

static bool g570Batch139BuildGpuPayload()
{
    constexpr int tileShift = 4;
    constexpr int tileSize = 1 << tileShift;
    constexpr int tilesX = static_cast<int>(kG403DisplayZW) / tileSize;
    constexpr int tilesY = static_cast<int>(kG403DisplayZH) / tileSize;
    constexpr size_t triStride = 20u;
    static_assert((kG403DisplayZW % tileSize) == 0u, "G570 tile width must divide surface");
    static_assert((kG403DisplayZH % tileSize) == 0u, "G570 tile height must divide surface");

    g_g570Batch139GpuTris.clear();
    g_g570Batch139GpuSpans.clear();
    g_g570Batch139GpuTileOffsets.clear();
    g_g570Batch139GpuTileTris.clear();
    g_g570Batch139GpuTris.reserve(g_g570Batch139Tris.size() * triStride);
    g_g570Batch139GpuSpans.reserve(g_g570Batch139Spans.size() * 4u);
    constexpr size_t tileCount = static_cast<size_t>(tilesX) * tilesY;
    std::array<uint32_t, tileCount> binCounts{};
    const bool tileCull = g570Batch139TileCullOn();

    const auto fitsI32 = [](int64_t value) {
        return value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
    };
    for (size_t triIndex = 0; triIndex < g_g570Batch139Tris.size(); ++triIndex)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[triIndex];
        const uint32_t gpuSpanOffset =
            static_cast<uint32_t>(g_g570Batch139GpuSpans.size() / 4u);
        if (!fitsI32(p.step[0]) || !fitsI32(p.step[1]) || !fitsI32(p.step[2]))
            return false;

        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.minX));
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.maxX));
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.minY));
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.maxY));
        g_g570Batch139GpuTris.push_back(gpuSpanOffset);
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[0]));
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[1]));
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[2]));
        const uint32_t packedMode = static_cast<uint32_t>(p.topLeftMask) |
            (p.delta > 0 ? 0x100u : 0u);
        g_g570Batch139GpuTris.push_back(packedMode);
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.a0));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.b0));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.a1));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.b1));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.fx2));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.fy2));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.winding));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.invAbsDenom));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z0));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z1));
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z2));

        if (p.maxX < p.minX)
            continue;
        const int64_t baseSx = static_cast<int64_t>(p.minX) * 16;
        const int64_t baseSy = static_cast<int64_t>(p.minY) * 16;
        int64_t rowEdge[3] = {
            ((p.fx[2] - p.fx[1]) * (baseSy - p.fy[1]) -
             (p.fy[2] - p.fy[1]) * (baseSx - p.fx[1])) * p.sign,
            ((p.fx[0] - p.fx[2]) * (baseSy - p.fy[2]) -
             (p.fy[0] - p.fy[2]) * (baseSx - p.fx[2])) * p.sign,
            ((p.fx[1] - p.fx[0]) * (baseSy - p.fy[0]) -
             (p.fy[1] - p.fy[0]) * (baseSx - p.fx[0])) * p.sign,
        };
        const int64_t rowStep[3] = {
            (p.fx[2] - p.fx[1]) * 16 * p.sign,
            (p.fx[0] - p.fx[2]) * 16 * p.sign,
            (p.fx[1] - p.fx[0]) * 16 * p.sign,
        };
        for (int y = p.minY; y <= p.maxY; ++y)
        {
            const uint32_t packedSpan = g_g570Batch139Spans[
                static_cast<size_t>(p.spanOffset) + static_cast<size_t>(y - p.minY)];
            const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
            const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
            const int64_t startDx = static_cast<int64_t>(xStart - p.minX);
            const int64_t edge0 = rowEdge[0] + p.step[0] * startDx;
            const int64_t edge1 = rowEdge[1] + p.step[1] * startDx;
            const int64_t edge2 = rowEdge[2] + p.step[2] * startDx;
            const int64_t dx = std::max(0, xEnd - xStart);
            const int64_t advance0 = p.step[0] * dx;
            const int64_t advance1 = p.step[1] * dx;
            const int64_t advance2 = p.step[2] * dx;
            if (!fitsI32(edge0) || !fitsI32(edge1) || !fitsI32(edge2) ||
                !fitsI32(advance0) || !fitsI32(advance1) || !fitsI32(advance2) ||
                !fitsI32(edge0 + advance0) || !fitsI32(edge1 + advance1) ||
                !fitsI32(edge2 + advance2))
                return false;
            g_g570Batch139GpuSpans.push_back(packedSpan);
            g_g570Batch139GpuSpans.push_back(static_cast<uint32_t>(edge0));
            g_g570Batch139GpuSpans.push_back(static_cast<uint32_t>(edge1));
            g_g570Batch139GpuSpans.push_back(static_cast<uint32_t>(edge2));
            rowEdge[0] += rowStep[0];
            rowEdge[1] += rowStep[1];
            rowEdge[2] += rowStep[2];
        }

        const int tx0 = p.minX >> tileShift;
        const int tx1 = p.maxX >> tileShift;
        const int ty0 = p.minY >> tileShift;
        const int ty1 = p.maxY >> tileShift;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
                if (!tileCull || g570Batch139TileMayCover(p, tx, ty))
                    ++binCounts[static_cast<size_t>(ty) * tilesX + tx];
    }

    g_g570Batch139GpuTileOffsets.resize(tileCount + 1u, 0u);
    for (size_t tile = 0; tile < tileCount; ++tile)
        g_g570Batch139GpuTileOffsets[tile + 1u] =
            g_g570Batch139GpuTileOffsets[tile] + binCounts[tile];
    g_g570Batch139GpuTileTris.resize(g_g570Batch139GpuTileOffsets.back());
    std::array<uint32_t, tileCount> cursors{};
    std::copy_n(g_g570Batch139GpuTileOffsets.begin(), tileCount, cursors.begin());
    for (size_t triIndex = 0; triIndex < g_g570Batch139Tris.size(); ++triIndex)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[triIndex];
        if (p.maxX < p.minX)
            continue;
        const int tx0 = p.minX >> tileShift;
        const int tx1 = p.maxX >> tileShift;
        const int ty0 = p.minY >> tileShift;
        const int ty1 = p.maxY >> tileShift;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                if (tileCull && !g570Batch139TileMayCover(p, tx, ty))
                    continue;
                const size_t tile = static_cast<size_t>(ty) * tilesX + tx;
                g_g570Batch139GpuTileTris[cursors[tile]++] =
                    static_cast<uint32_t>(triIndex);
            }
    }
    if (g_g570Batch139GpuTris.empty() || g_g570Batch139GpuSpans.empty() ||
        g_g570Batch139GpuTileTris.empty())
        return false;
    g570Batch139PadGpuWords(g_g570Batch139GpuTris);
    g570Batch139PadGpuWords(g_g570Batch139GpuSpans);
    g570Batch139PadGpuWords(g_g570Batch139GpuTileOffsets);
    g570Batch139PadGpuWords(g_g570Batch139GpuTileTris);
    return true;
}

inline void g570Batch139ComputeSpan(const G570Batch139Tri &p, int y,
                                    int &xStart, int &xEnd)
{
    xStart = p.minX;
    xEnd = p.maxX;
    static const bool noXspan = std::getenv("DC2_G141_NO_XSPAN") != nullptr;
    if (noXspan)
        return;
    const double s0 = static_cast<double>(p.a0) * static_cast<double>(p.winding) *
                      static_cast<double>(p.invAbsDenom);
    const double s1 = static_cast<double>(p.a1) * static_cast<double>(p.winding) *
                      static_cast<double>(p.invAbsDenom);
    const double s2 = -s0 - s1;
    const double eps = static_cast<double>(1.0e-4f);
    const double basePx = static_cast<double>(p.minX) + 0.5;
    const float pyf = static_cast<float>(y) + 0.5f;
    const double pyd = static_cast<double>(pyf);
    const double w0b = ((static_cast<double>(p.a0) *
                         (basePx - static_cast<double>(p.fx2)) +
                         static_cast<double>(p.b0) *
                         (pyd - static_cast<double>(p.fy2))) *
                        static_cast<double>(p.winding) *
                        static_cast<double>(p.invAbsDenom));
    const double w1b = ((static_cast<double>(p.a1) *
                         (basePx - static_cast<double>(p.fx2)) +
                         static_cast<double>(p.b1) *
                         (pyd - static_cast<double>(p.fy2))) *
                        static_cast<double>(p.winding) *
                        static_cast<double>(p.invAbsDenom));
    const double w2b = 1.0 - w0b - w1b;
    double tLo = 0.0;
    double tHi = static_cast<double>(p.maxX - p.minX);
    const double tiny = 1e-9;
    const double lo0 = -eps - w0b, lo1 = -eps - w1b, lo2 = -eps - w2b;
    if (s0 > tiny)       { const double c = lo0 / s0; if (c > tLo) tLo = c; }
    else if (s0 < -tiny) { const double c = lo0 / s0; if (c < tHi) tHi = c; }
    if (s1 > tiny)       { const double c = lo1 / s1; if (c > tLo) tLo = c; }
    else if (s1 < -tiny) { const double c = lo1 / s1; if (c < tHi) tHi = c; }
    if (s2 > tiny)       { const double c = lo2 / s2; if (c > tLo) tLo = c; }
    else if (s2 < -tiny) { const double c = lo2 / s2; if (c < tHi) tHi = c; }
    double tLoC = std::floor(tLo) - 1.0;
    if (tLoC < 0.0) tLoC = 0.0;
    double tHiC = std::ceil(tHi) + 1.0;
    const double span = static_cast<double>(p.maxX - p.minX);
    if (tHiC > span) tHiC = span;
    xStart = p.minX + static_cast<int>(tLoC);
    xEnd = p.minX + static_cast<int>(tHiC);
}

inline int64_t g570Batch139FloorDiv(int64_t value, int64_t positiveDivisor)
{
    const int64_t q = value / positiveDivisor;
    const int64_t r = value % positiveDivisor;
    return q - (r < 0 ? 1 : 0);
}

// Convert the same three fixed-point edge tests used by the exact shader into their minimal
// integer-X interval.  Triangle coverage is convex, so intersecting these three half-lines loses
// no covered pixel and lets rejected pixels skip the shader's float interpolation/depth work.
struct G570Batch139SpanBound
{
    int64_t divisor = 1;
    int64_t quotient = 0;
    int64_t remainder = 0;
    int64_t deltaQuotient = 0;
    int64_t deltaRemainder = 0;
    bool lower = false;

    void reset(int64_t numerator, int64_t delta, int64_t positiveDivisor, bool isLower)
    {
        divisor = positiveDivisor;
        quotient = g570Batch139FloorDiv(numerator, divisor);
        remainder = numerator - quotient * divisor;
        deltaQuotient = g570Batch139FloorDiv(delta, divisor);
        deltaRemainder = delta - deltaQuotient * divisor;
        lower = isLower;
    }

    int64_t value() const
    {
        return quotient + (lower && remainder != 0 ? 1 : 0);
    }

    void advance()
    {
        quotient += deltaQuotient;
        remainder += deltaRemainder;
        if (remainder >= divisor)
        {
            remainder -= divisor;
            ++quotient;
        }
    }
};

inline void g570Batch139ExactSpan(const G570Batch139Tri &p, const int64_t rowEdge[3],
                                  const G570Batch139SpanBound bounds[3],
                                  int &xStart, int &xEnd)
{
    int64_t lo = 0;
    int64_t hi = static_cast<int64_t>(p.maxX - p.minX);
    for (int edge = 0; edge < 3 && lo <= hi; ++edge)
    {
        const int64_t threshold = (p.topLeftMask & (1u << edge)) != 0u ? 0 : 1;
        const int64_t step = p.step[edge];
        if (step > 0)
            lo = std::max(lo, bounds[edge].value());
        else if (step < 0)
            hi = std::min(hi, bounds[edge].value());
        else if (rowEdge[edge] < threshold)
            lo = hi + 1;
    }
    if (lo > hi)
    {
        xStart = p.minX;
        xEnd = p.minX - 1;
        return;
    }
    xStart = p.minX + static_cast<int>(lo);
    xEnd = p.minX + static_cast<int>(hi);
}

static bool g570Batch139Prepare()
{
    g_g570Batch139RejectReason = 1;
    g_g570Batch139Tris.clear();
    g_g570Batch139Spans.clear();
    g_g570Batch139GpuTris.clear();
    g_g570Batch139GpuSpans.clear();
    g_g570Batch139GpuTileOffsets.clear();
    g_g570Batch139GpuTileTris.clear();
    if (!g570Batch139On() || !g570Cpu139Quiet() || g570Cpu139On() ||
        envFlagEnabled("DC2_G408_LEGACY_SHADOW_COVERAGE") ||
        !g203UniversalZEnabled() || g_g144LastGs == nullptr ||
        g_g144List.size() < 2u)
        return false;

    g_g570Batch139RejectReason = 2;
    const G144Entry &clear = g_g144List.front();
    const auto frameIs139 = [](const G144Entry &e) {
        return e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
               e.ctx.frame.psm == GS_PSM_CT32;
    };
    if (!frameIs139(clear) || clear.prim.type != GS_PRIM_SPRITE ||
        clear.prim.tme || clear.prim.abe || clear.prim.fge ||
        clear.ctx.test != 0x3000aull || clear.ctx.zbuf != 0x1010000d0ull)
        return false;
    g_g570Batch139RejectReason = 3;
    if (!g404SharedZScope(0x139u, 8u, 0x0d0u, 1u))
        return false;

    const bool gpuSpans = g570Batch139GpuOn();
    const bool tightGpuSpans = gpuSpans && g570Batch139TightGpuSpansOn();
    g_g570Batch139Tris.reserve(g_g144List.size() - 1u);
    for (size_t i = 1; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        const uint8_t alphaMode = static_cast<uint8_t>(e.ctx.alpha & 0xffu);
        const GSVertex &v0 = e.v[0], &v1 = e.v[1], &v2 = e.v[2];
        const bool flatUnit = !e.prim.iip && v2.r == 1u && v2.g == 1u &&
                              v2.b == 1u && v2.a == 0x40u;
        if (!frameIs139(e) || e.prim.type != GS_PRIM_TRIFAN || e.prim.tme ||
            !e.prim.abe || e.prim.fge || e.ctx.test != 0x5000aull ||
            e.ctx.zbuf != 0x1010000d0ull ||
            (alphaMode != 0x62u && alphaMode != 0x68u) ||
            static_cast<uint8_t>((e.ctx.alpha >> 32u) & 0xffu) != 0x80u ||
            !flatUnit || e.ctx.frame.fbmsk != 0u || e.ctx.fba != 0u || e.pabe)
        {
            g_g570Batch139RejectReason = 4;
            g_g570Batch139Tris.clear();
            g_g570Batch139Spans.clear();
            return false;
        }

        const int ofx = e.ctx.xyoffset.ofx >> 4;
        const int ofy = e.ctx.xyoffset.ofy >> 4;
        const float x0 = v0.x - static_cast<float>(ofx);
        const float y0 = v0.y - static_cast<float>(ofy);
        const float x1 = v1.x - static_cast<float>(ofx);
        const float y1 = v1.y - static_cast<float>(ofy);
        const float x2 = v2.x - static_cast<float>(ofx);
        const float y2 = v2.y - static_cast<float>(ofy);
        const float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);

        G570Batch139Tri p;
        p.ofx = ofx; p.ofy = ofy;
        p.rawX[0] = v0.x; p.rawX[1] = v1.x; p.rawX[2] = v2.x;
        p.rawY[0] = v0.y; p.rawY[1] = v1.y; p.rawY[2] = v2.y;
        p.minX = std::max(static_cast<int>(e.ctx.scissor.x0),
                          static_cast<int>(std::floor(std::min({x0, x1, x2}))));
        p.maxX = std::min(static_cast<int>(e.ctx.scissor.x1),
                          static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
        p.minY = std::max(static_cast<int>(e.ctx.scissor.y0),
                          static_cast<int>(std::floor(std::min({y0, y1, y2}))));
        p.maxY = std::min(static_cast<int>(e.ctx.scissor.y1),
                          static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
        if (p.minX < 0 || p.maxX >= static_cast<int>(kG403DisplayZW) ||
            p.minY < 0 || p.maxY >= static_cast<int>(kG403DisplayZH))
        {
            g_g570Batch139RejectReason = 5;
            g_g570Batch139Tris.clear();
            g_g570Batch139Spans.clear();
            return false;
        }
        p.delta = (alphaMode == 0x68u) ? 1 : -1;
        if (std::fabs(denom) >= 0.001f)
        {
            p.fx[0] = static_cast<int64_t>(std::llround(x0 * 16.0f));
            p.fx[1] = static_cast<int64_t>(std::llround(x1 * 16.0f));
            p.fx[2] = static_cast<int64_t>(std::llround(x2 * 16.0f));
            p.fy[0] = static_cast<int64_t>(std::llround(y0 * 16.0f));
            p.fy[1] = static_cast<int64_t>(std::llround(y1 * 16.0f));
            p.fy[2] = static_cast<int64_t>(std::llround(y2 * 16.0f));
            p.sign = denom >= 0.0f ? 1 : -1;
            const auto topLeft = [&](int a, int b) {
                int64_t dx = p.fx[b] - p.fx[a];
                int64_t dy = p.fy[b] - p.fy[a];
                if (p.sign < 0) { dx = -dx; dy = -dy; }
                return dy < 0 || (dy == 0 && dx > 0);
            };
            if (topLeft(1, 2)) p.topLeftMask |= 1u;
            if (topLeft(2, 0)) p.topLeftMask |= 2u;
            if (topLeft(0, 1)) p.topLeftMask |= 4u;
            p.step[0] = -(p.fy[2] - p.fy[1]) * 16 * p.sign;
            p.step[1] = -(p.fy[0] - p.fy[2]) * 16 * p.sign;
            p.step[2] = -(p.fy[1] - p.fy[0]) * 16 * p.sign;
            p.rowStep[0] = (p.fx[2] - p.fx[1]) * 16 * p.sign;
            p.rowStep[1] = (p.fx[0] - p.fx[2]) * 16 * p.sign;
            p.rowStep[2] = (p.fx[1] - p.fx[0]) * 16 * p.sign;
            p.a0 = y1 - y2; p.b0 = x2 - x1;
            p.a1 = y2 - y0; p.b1 = x0 - x2;
            p.fx2 = x2; p.fy2 = y2;
            p.winding = denom < 0.0f ? -1.0f : 1.0f;
            p.invAbsDenom = 1.0f / std::fabs(denom);
            p.z0 = v0.z; p.z1 = v1.z; p.z2 = v2.z;
            p.spanOffset = static_cast<uint32_t>(g_g570Batch139Spans.size());
            const int64_t baseSx = static_cast<int64_t>(p.minX) * 16;
            const int64_t baseSy = static_cast<int64_t>(p.minY) * 16;
            p.edgeBase[0] =
                ((p.fx[2] - p.fx[1]) * (baseSy - p.fy[1]) -
                 (p.fy[2] - p.fy[1]) * (baseSx - p.fx[1])) * p.sign;
            p.edgeBase[1] =
                ((p.fx[0] - p.fx[2]) * (baseSy - p.fy[2]) -
                 (p.fy[0] - p.fy[2]) * (baseSx - p.fx[2])) * p.sign;
            p.edgeBase[2] =
                ((p.fx[1] - p.fx[0]) * (baseSy - p.fy[0]) -
                 (p.fy[1] - p.fy[0]) * (baseSx - p.fx[0])) * p.sign;
            int64_t rowEdge[3] = {
                p.edgeBase[0], p.edgeBase[1], p.edgeBase[2],
            };
            const int64_t rowStep[3] = {
                p.rowStep[0], p.rowStep[1], p.rowStep[2],
            };
            G570Batch139SpanBound spanBounds[3];
            if (tightGpuSpans)
            {
                for (int edge = 0; edge < 3; ++edge)
                {
                    const int64_t threshold =
                        (p.topLeftMask & (1u << edge)) != 0u ? 0 : 1;
                    if (p.step[edge] > 0)
                        spanBounds[edge].reset(threshold - rowEdge[edge], -rowStep[edge],
                                               p.step[edge], true);
                    else if (p.step[edge] < 0)
                        spanBounds[edge].reset(rowEdge[edge] - threshold, rowStep[edge],
                                               -p.step[edge], false);
                }
            }
            for (int y = p.minY; y <= p.maxY; ++y)
            {
                int xStart = p.minX, xEnd = p.maxX;
                if (tightGpuSpans)
                    g570Batch139ExactSpan(p, rowEdge, spanBounds, xStart, xEnd);
                else if (!gpuSpans)
                    g570Batch139ComputeSpan(p, y, xStart, xEnd);
                g_g570Batch139Spans.push_back(
                    static_cast<uint32_t>(xStart & 0xffff) |
                    (static_cast<uint32_t>(xEnd & 0xffff) << 16u));
                rowEdge[0] += rowStep[0];
                rowEdge[1] += rowStep[1];
                rowEdge[2] += rowStep[2];
                if (tightGpuSpans)
                {
                    spanBounds[0].advance();
                    spanBounds[1].advance();
                    spanBounds[2].advance();
                }
            }
        }
        else
        {
            p.maxX = -1; // generic drawTriangle drops the same degenerate primitive
        }
        g_g570Batch139Tris.push_back(p);
    }
    if (g_g570Batch139Tris.empty())
    {
        g_g570Batch139RejectReason = 6;
        return false;
    }
    if (g570Batch139GpuOn() && !g570Batch139BuildGpuPayload())
    {
        g_g570Batch139RejectReason = 7;
        return false;
    }
    g_g570Batch139RejectReason = 0;
    return true;
}

struct G570Batch139LaneCache
{
    std::vector<uint32_t> value;
    std::vector<uint32_t> stamp;
    std::vector<uint32_t> offset;
    std::vector<uint32_t> dirty;
    std::vector<G530RowAddr> rows;
    uint32_t generation = 0u;
    int y0 = 0;
};

thread_local G570Batch139LaneCache t_g570Batch139Cache;

static uint64_t g570Batch139ReplayRange(uint8_t *vram, size_t vramSize, int y0, int y1,
                                        size_t firstTri, size_t lastTri)
{
    if (vram == nullptr || y0 < 0 || y1 < y0 || y1 >= static_cast<int>(kG403DisplayZH))
        return 0u;
    constexpr int width = static_cast<int>(kG403DisplayZW);
    constexpr size_t surfacePixels = static_cast<size_t>(kG403DisplayZW) * kG403DisplayZH;
    constexpr uint32_t frameBlock = 0x139u << 5u;
    G570Batch139LaneCache &cache = t_g570Batch139Cache;
    if (cache.value.size() != surfacePixels)
    {
        cache.value.resize(surfacePixels);
        cache.stamp.assign(surfacePixels, 0u);
        cache.offset.resize(surfacePixels);
    }
    if (++cache.generation == 0u)
    {
        std::fill(cache.stamp.begin(), cache.stamp.end(), 0u);
        cache.generation = 1u;
    }
    cache.y0 = y0;
    cache.dirty.clear();
    cache.rows.resize(static_cast<size_t>(y1 - y0 + 1));
    for (int y = y0; y <= y1; ++y)
        cache.rows[static_cast<size_t>(y - y0)] =
            g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));

    auto touch = [&](int x, int y) -> size_t {
        const size_t index = static_cast<size_t>(y) * width + static_cast<size_t>(x);
        if (cache.stamp[index] != cache.generation)
        {
            const G530RowAddr &row = cache.rows[static_cast<size_t>(y - cache.y0)];
            const uint32_t groupBase = g530GroupBase(row, static_cast<uint32_t>(x));
            const uint32_t off = groupBase + static_cast<uint32_t>(row.ct[x & 7]) * 4u;
            cache.offset[index] = off;
            uint32_t word = 0u;
            if (off + 4u <= vramSize)
                std::memcpy(&word, vram + off, sizeof(word));
            cache.value[index] = word;
            cache.stamp[index] = cache.generation;
            cache.dirty.push_back(static_cast<uint32_t>(index));
        }
        return index;
    };

    uint64_t accepted = 0u;
    const __m128 oneF = _mm_set1_ps(1.0f);
    const __m128 zeroF = _mm_setzero_ps();
    const __m128 zMaxF = _mm_set1_ps(16777215.0f);
    const __m128i oneRgb = _mm_set1_epi32(0x00010101);
    const __m128i alpha = _mm_set1_epi32(0x40000000);
    const __m128i rgbMask = _mm_set1_epi32(0x00ffffff);
    const uint32_t *depth = g403DisplayZBuf().data();
    static const bool immediate = envFlagEnabled("DC2_G570_BATCH_IMMEDIATE");
    lastTri = std::min(lastTri, g_g570Batch139Tris.size());
    for (size_t triIndex = firstTri; triIndex < lastTri; ++triIndex)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[triIndex];
        if (p.maxX < p.minX)
            continue;
        const int firstY = std::max(y0, p.minY);
        const int lastY = std::min(y1, p.maxY);
        if (lastY < firstY)
            continue;

        // Recreate the float setup on the lane thread, exactly where drawTriangle would.  The GS
        // worker and row-pool threads can carry different MXCSR state; persisting these values
        // across that boundary caused rare GEQUAL boundary differences.
        const float lx0 = p.rawX[0] - static_cast<float>(p.ofx);
        const float ly0 = p.rawY[0] - static_cast<float>(p.ofy);
        const float lx1 = p.rawX[1] - static_cast<float>(p.ofx);
        const float ly1 = p.rawY[1] - static_cast<float>(p.ofy);
        const float lx2 = p.rawX[2] - static_cast<float>(p.ofx);
        const float ly2 = p.rawY[2] - static_cast<float>(p.ofy);
        const float laneDenom = (ly1 - ly2) * (lx0 - lx2) +
                                (lx2 - lx1) * (ly0 - ly2);
        const float laneA0 = ly1 - ly2, laneB0 = lx2 - lx1;
        const float laneA1 = ly2 - ly0, laneB1 = lx0 - lx2;
        const float laneWinding = laneDenom < 0.0f ? -1.0f : 1.0f;
        const float laneInvDenom = 1.0f / std::fabs(laneDenom);
        const __m128 a0 = _mm_set1_ps(laneA0), b0 = _mm_set1_ps(laneB0);
        const __m128 a1 = _mm_set1_ps(laneA1), b1 = _mm_set1_ps(laneB1);
        const __m128 fx2 = _mm_set1_ps(lx2);
        const __m128 winding = _mm_set1_ps(laneWinding);
        const __m128 invDenom = _mm_set1_ps(laneInvDenom);
        const __m128 z0 = _mm_set1_ps(p.z0), z1 = _mm_set1_ps(p.z1), z2 = _mm_set1_ps(p.z2);
        for (int y = firstY; y <= lastY; ++y)
        {
            const uint32_t packedSpan = g_g570Batch139Spans[
                static_cast<size_t>(p.spanOffset) + static_cast<size_t>(y - p.minY)];
            const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
            const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
            const int64_t sx = static_cast<int64_t>(xStart) * 16;
            const int64_t sy = static_cast<int64_t>(y) * 16;
            int64_t edge0 = ((p.fx[2] - p.fx[1]) * (sy - p.fy[1]) -
                             (p.fy[2] - p.fy[1]) * (sx - p.fx[1])) * p.sign;
            int64_t edge1 = ((p.fx[0] - p.fx[2]) * (sy - p.fy[2]) -
                             (p.fy[0] - p.fy[2]) * (sx - p.fx[2])) * p.sign;
            int64_t edge2 = ((p.fx[1] - p.fx[0]) * (sy - p.fy[0]) -
                             (p.fy[1] - p.fy[0]) * (sx - p.fx[0])) * p.sign;
            __m128 px = _mm_setr_ps(static_cast<float>(xStart),
                                    static_cast<float>(xStart + 1),
                                    static_cast<float>(xStart + 2),
                                    static_cast<float>(xStart + 3));
            const __m128 dy2 = _mm_set1_ps(static_cast<float>(y) - ly2);
            bool entered = false;
            for (int x = xStart; x <= xEnd; x += 4, px = _mm_add_ps(px, _mm_set1_ps(4.0f)))
            {
                const int lanes = std::min(4, xEnd - x + 1);
                uint32_t coverage = 0u;
                for (int lane = 0; lane < lanes; ++lane)
                {
                    if (!(edge0 < 0 || (edge0 == 0 && (p.topLeftMask & 1u) == 0u) ||
                          edge1 < 0 || (edge1 == 0 && (p.topLeftMask & 2u) == 0u) ||
                          edge2 < 0 || (edge2 == 0 && (p.topLeftMask & 4u) == 0u)))
                        coverage |= 1u << lane;
                    edge0 += p.step[0]; edge1 += p.step[1]; edge2 += p.step[2];
                }
                if (coverage == 0u)
                {
                    if (entered) break; // convex triangle: coverage cannot resume on this row
                    continue;
                }
                entered = true;
                if (g570Batch139GpuVerifyOn())
                {
                    for (int lane = 0; lane < lanes; ++lane)
                        if ((coverage & (1u << lane)) != 0u)
                        {
                            const size_t debugIndex = static_cast<size_t>(y) * width +
                                                      static_cast<size_t>(x + lane);
                            uint32_t debug = g_g570Batch139CpuDebug[debugIndex];
                            const uint32_t hash =
                                (((debug & 0xffffu) * 251u) +
                                 static_cast<uint32_t>(triIndex) + 1u) & 0xffffu;
                            g_g570Batch139CpuDebug[debugIndex] =
                                (debug & 0xffff0000u) | hash;
                        }
                }

                __m128 w0 = _mm_add_ps(_mm_mul_ps(a0, _mm_sub_ps(px, fx2)),
                                       _mm_mul_ps(b0, dy2));
                w0 = _mm_mul_ps(_mm_mul_ps(w0, winding), invDenom);
                __m128 w1 = _mm_add_ps(_mm_mul_ps(a1, _mm_sub_ps(px, fx2)),
                                       _mm_mul_ps(b1, dy2));
                w1 = _mm_mul_ps(_mm_mul_ps(w1, winding), invDenom);
                const __m128 w2 = _mm_sub_ps(_mm_sub_ps(oneF, w0), w1);
                __m128 zf = _mm_add_ps(_mm_add_ps(_mm_mul_ps(z0, w0), _mm_mul_ps(z1, w1)),
                                       _mm_mul_ps(z2, w2));
                zf = _mm_min_ps(_mm_max_ps(zf, zeroF), zMaxF);
                const __m128i zi = _mm_cvttps_epi32(zf);
                alignas(16) uint32_t zd[4]{};
                for (int lane = 0; lane < lanes; ++lane)
                    zd[lane] = depth[static_cast<size_t>(y) * width + x + lane];
                const __m128i zdst = _mm_load_si128(reinterpret_cast<const __m128i *>(zd));
                const __m128i passV = _mm_or_si128(_mm_cmpgt_epi32(zi, zdst),
                                                   _mm_cmpeq_epi32(zi, zdst));
                uint32_t pass = coverage & static_cast<uint32_t>(
                    _mm_movemask_ps(_mm_castsi128_ps(passV)));
                if (lanes < 4) pass &= (1u << lanes) - 1u;
                if (g570Batch139VerifyOn())
                {
                    for (int lane = 0; lane < lanes; ++lane)
                    {
                        if ((coverage & (1u << lane)) == 0u) continue;
                        const float pxf = static_cast<float>(x + lane);
                        const float pyf = static_cast<float>(y);
                        const float rw0 = (((laneA0 * (pxf - lx2) +
                                             laneB0 * (pyf - ly2)) * laneWinding) *
                                           laneInvDenom);
                        const float rw1 = (((laneA1 * (pxf - lx2) +
                                             laneB1 * (pyf - ly2)) * laneWinding) *
                                           laneInvDenom);
                        const float rw2 = 1.0f - rw0 - rw1;
                        float rzf = p.z0 * rw0 + p.z1 * rw1 + p.z2 * rw2;
                        if (rzf < 0.0f) rzf = 0.0f;
                        if (rzf > 16777215.0f) rzf = 16777215.0f;
                        const bool referencePass = static_cast<uint32_t>(rzf) >= zd[lane];
                        const bool candidatePass = (pass & (1u << lane)) != 0u;
                        if (referencePass != candidatePass)
                        {
                            static std::atomic<uint32_t> s_g570DepthBad{0u};
                            const uint32_t k = s_g570DepthBad.fetch_add(
                                1u, std::memory_order_relaxed);
                            if (k < 32u)
                                std::fprintf(stderr,
                                    "[G570:batchdepthbad] k=%u xy=(%d,%d) z=%u zd=%u "
                                    "candidate=%u reference=%u\n",
                                    k, x + lane, y, static_cast<uint32_t>(rzf), zd[lane],
                                    candidatePass ? 1u : 0u, referencePass ? 1u : 0u);
                        }
                    }
                }
                if (pass == 0u)
                    continue;

                if (g570Batch139GpuVerifyOn())
                {
                    for (int lane = 0; lane < lanes; ++lane)
                        if ((pass & (1u << lane)) != 0u)
                        {
                            const size_t debugIndex = static_cast<size_t>(y) * width +
                                                      static_cast<size_t>(x + lane);
                            uint32_t debug = g_g570Batch139CpuDebug[debugIndex];
                            const uint32_t hash =
                                ((((debug >> 16u) & 0xffffu) * 251u) +
                                 static_cast<uint32_t>(triIndex) + 1u) & 0xffffu;
                            g_g570Batch139CpuDebug[debugIndex] =
                                (debug & 0xffffu) | (hash << 16u);
                        }
                }

                alignas(16) uint32_t words[4]{};
                size_t indices[4]{};
                uint32_t immediateOff[4]{};
                for (int lane = 0; lane < lanes; ++lane)
                {
                    if ((pass & (1u << lane)) == 0u) continue;
                    if (immediate)
                    {
                        const G530RowAddr &row =
                            cache.rows[static_cast<size_t>(y - cache.y0)];
                        immediateOff[lane] = g530GroupBase(
                            row, static_cast<uint32_t>(x + lane)) +
                            static_cast<uint32_t>(row.ct[(x + lane) & 7]) * 4u;
                        if (immediateOff[lane] + 4u <= vramSize)
                            std::memcpy(&words[lane], vram + immediateOff[lane],
                                        sizeof(uint32_t));
                    }
                    else
                    {
                        indices[lane] = touch(x + lane, y);
                        words[lane] = cache.value[indices[lane]];
                    }
                }
                const __m128i old = _mm_load_si128(reinterpret_cast<const __m128i *>(words));
                const __m128i adjusted = p.delta > 0 ? _mm_adds_epu8(old, oneRgb)
                                                     : _mm_subs_epu8(old, oneRgb);
                const __m128i result = _mm_or_si128(_mm_and_si128(adjusted, rgbMask), alpha);
                _mm_store_si128(reinterpret_cast<__m128i *>(words), result);
                for (int lane = 0; lane < lanes; ++lane)
                    if ((pass & (1u << lane)) != 0u)
                    {
                        if (immediate)
                        {
                            if (immediateOff[lane] + 4u <= vramSize)
                                std::memcpy(vram + immediateOff[lane], &words[lane],
                                            sizeof(uint32_t));
                        }
                        else
                        {
                            cache.value[indices[lane]] = words[lane];
                        }
                        ++accepted;
                    }
            }
        }
    }

    for (uint32_t rawIndex : cache.dirty)
    {
        const size_t index = static_cast<size_t>(rawIndex);
        const uint32_t off = cache.offset[index];
        if (off + 4u <= vramSize)
            std::memcpy(vram + off, &cache.value[index], sizeof(uint32_t));
    }
    return accepted;
}

static uint64_t g570Batch139ReplayBand(uint8_t *vram, size_t vramSize, int y0, int y1)
{
    return g570Batch139ReplayRange(vram, vramSize, y0, y1,
                                   0u, g_g570Batch139Tris.size());
}

static uint64_t g570Batch139ReplayTri(uint8_t *vram, size_t vramSize,
                                     int y0, int y1, size_t triIndex)
{
    return g570Batch139ReplayRange(vram, vramSize, y0, y1,
                                   triIndex, triIndex + 1u);
}

static size_t g570Batch139TriCount()
{
    return g_g570Batch139Tris.size();
}

static void g570Batch139ReadBand(const uint8_t *vram, size_t vramSize,
                                 int y0, int y1, std::vector<uint32_t> &out)
{
    constexpr int width = static_cast<int>(kG403DisplayZW);
    constexpr uint32_t frameBlock = 0x139u << 5u;
    out.assign(static_cast<size_t>(y1 - y0 + 1) * width, 0u);
    for (int y = y0; y <= y1; ++y)
    {
        const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
        for (int x = 0; x < width; ++x)
        {
            const uint32_t off = g530GroupBase(row, static_cast<uint32_t>(x)) +
                                 static_cast<uint32_t>(row.ct[x & 7]) * 4u;
            if (off + 4u <= vramSize)
                std::memcpy(&out[static_cast<size_t>(y - y0) * width + x],
                            vram + off, sizeof(uint32_t));
        }
    }
}

static void g570Batch139WriteBand(uint8_t *vram, size_t vramSize,
                                  int y0, int y1, const std::vector<uint32_t> &in)
{
    constexpr int width = static_cast<int>(kG403DisplayZW);
    constexpr uint32_t frameBlock = 0x139u << 5u;
    if (in.size() < static_cast<size_t>(y1 - y0 + 1) * width)
        return;
    for (int y = y0; y <= y1; ++y)
    {
        const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
        for (int x = 0; x < width; ++x)
        {
            const uint32_t off = g530GroupBase(row, static_cast<uint32_t>(x)) +
                                 static_cast<uint32_t>(row.ct[x & 7]) * 4u;
            if (off + 4u <= vramSize)
                std::memcpy(vram + off,
                            &in[static_cast<size_t>(y - y0) * width + x], sizeof(uint32_t));
        }
    }
}

static bool g570Batch139ReplayGpu(const std::vector<uint32_t> &color,
                                  std::vector<uint32_t> &out)
{
    constexpr size_t surfacePixels =
        static_cast<size_t>(kG403DisplayZW) * kG403DisplayZH;
    if (!g570Batch139GpuOn() || color.size() != surfacePixels ||
        g403DisplayZBuf().size() != surfacePixels ||
        g_g570Batch139GpuTris.empty() || g_g570Batch139GpuSpans.empty() ||
        g_g570Batch139GpuTileOffsets.empty() || g_g570Batch139GpuTileTris.empty())
        return false;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = g570_backend_shadow139(
        color, g403DisplayZBuf(), g_g570Batch139GpuTris,
        g_g570Batch139GpuSpans, g_g570Batch139GpuTileOffsets,
        g_g570Batch139GpuTileTris, out);
    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    static std::atomic<uint64_t> s_batches{0u}, s_us{0u}, s_failed{0u};
    const uint64_t batches = s_batches.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t totalUs = s_us.fetch_add(us, std::memory_order_relaxed) + us;
    const uint64_t failed = !ok
        ? s_failed.fetch_add(1u, std::memory_order_relaxed) + 1u
        : s_failed.load(std::memory_order_relaxed);
    if (batches <= 8u || (batches % 120u) == 0u || !ok)
        std::fprintf(stderr,
                     "[G570:gpu139] batches=%llu avg=%.3fms last=%.3fms failed=%llu\n",
                     static_cast<unsigned long long>(batches),
                     static_cast<double>(totalUs) / 1000.0 / batches,
                     static_cast<double>(us) / 1000.0,
                     static_cast<unsigned long long>(failed));
    return ok && out.size() == surfacePixels;
}

std::atomic<uint64_t> g_g570Batch139Bands{0u};
std::atomic<uint64_t> g_g570Batch139Pixels{0u};
std::atomic<uint64_t> g_g570Batch139Bad{0u};
std::atomic<uint64_t> g_g570Batch139GpuBands{0u};
std::atomic<uint64_t> g_g570Batch139GpuPixels{0u};
std::atomic<uint64_t> g_g570Batch139GpuBad{0u};

static void g570Batch139NoteVerify(uint64_t pixels, uint64_t bad)
{
    const uint64_t bands = g_g570Batch139Bands.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t totalPixels =
        g_g570Batch139Pixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
    const uint64_t totalBad = g_g570Batch139Bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bands <= 8u || (bands % 240u) == 0u || bad != 0u)
        std::fprintf(stderr, "[G570:batch139] bands=%llu pixels=%llu bad=%llu mode=verify\n",
                     static_cast<unsigned long long>(bands),
                     static_cast<unsigned long long>(totalPixels),
                     static_cast<unsigned long long>(totalBad));
}

static void g570Batch139NoteGpuVerify(uint64_t pixels, uint64_t bad)
{
    const uint64_t bands =
        g_g570Batch139GpuBands.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t totalPixels =
        g_g570Batch139GpuPixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
    const uint64_t totalBad =
        g_g570Batch139GpuBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bands <= 8u || (bands % 60u) == 0u || bad != 0u)
        std::fprintf(stderr,
                     "[G570:gpuverify] batches=%llu pixels=%llu bad=%llu\n",
                     static_cast<unsigned long long>(bands),
                     static_cast<unsigned long long>(totalPixels),
                     static_cast<unsigned long long>(totalBad));
}

constexpr int kG570Linear139W = 512;
constexpr int kG570Linear139H = 512;

struct G570Linear139Lane
{
    bool active = false;
    bool delta = false;
    int y0 = 0;
    int y1 = -1;
};

thread_local G570Linear139Lane t_g570Linear139;

inline std::vector<uint32_t> &g570Linear139Surface()
{
    static std::vector<uint32_t> s_pixels(
        static_cast<size_t>(kG570Linear139W) * kG570Linear139H, 0u);
    return s_pixels;
}

inline std::vector<uint8_t> &g570Delta139Surface()
{
    static std::vector<uint8_t> s_level(
        static_cast<size_t>(kG570Linear139W) * kG570Linear139H, 0u);
    return s_level;
}

inline std::vector<uint8_t> &g570Delta139Alpha()
{
    static std::vector<uint8_t> s_alpha(
        static_cast<size_t>(kG570Linear139W) * kG570Linear139H, 0u);
    return s_alpha;
}

inline void g570Delta139Accumulate(int x, int y, int delta, uint8_t alpha)
{
    const size_t i = static_cast<size_t>(y) * kG570Linear139W +
                     static_cast<size_t>(x);
    uint8_t &level = g570Delta139Surface()[i];
    // Preserve the GS's per-fragment saturation order exactly.  This is still much cheaper than
    // unpacking, blending and clamping three identical channels on every shadow-volume hit.
    if (delta > 0)
    {
        if (level != 0xffu)
            ++level;
    }
    else if (level != 0u)
    {
        --level;
    }
    // Zero means untouched. The admitted source alpha is exactly 0x40, so it is also the final
    // destination alpha after any accepted shadow fragment.
    g570Delta139Alpha()[i] = alpha;
}

inline uint32_t g570Linear139Read(int x, int y)
{
    return g570Linear139Surface()[static_cast<size_t>(y) * kG570Linear139W +
                                  static_cast<size_t>(x)];
}

inline void g570Linear139Write(int x, int y, uint32_t pixel)
{
    g570Linear139Surface()[static_cast<size_t>(y) * kG570Linear139W +
                           static_cast<size_t>(x)] = pixel;
}

std::atomic<uint64_t> g_g570Linear139Bands{0u};
std::atomic<uint64_t> g_g570Linear139Pixels{0u};
std::atomic<uint64_t> g_g570Linear139Bad{0u};

static bool g570Cpu139LinearBatchCandidate()
{
    if ((!g570Cpu139LinearOn() && !g570Cpu139DeltaOn()) || !g570Cpu139Quiet() ||
        !g203UniversalZEnabled() || g_g144List.size() < 2u ||
        g_g144LastGs == nullptr)
        return false;

    const auto frameIs139 = [](const G144Entry &e) {
        return e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
               e.ctx.frame.psm == GS_PSM_CT32;
    };
    const G144Entry &clear = g_g144List.front();
    if (!frameIs139(clear) || clear.prim.type != GS_PRIM_SPRITE ||
        clear.prim.tme || clear.prim.abe || clear.prim.fge ||
        clear.ctx.test != 0x3000aull || clear.ctx.zbuf != 0x1010000d0ull)
        return false;

    for (size_t i = 1; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        if (!frameIs139(e) || e.prim.type != GS_PRIM_TRIFAN || e.prim.tme ||
            !e.prim.abe || e.prim.fge || e.ctx.test != 0x5000aull ||
            e.ctx.zbuf != 0x1010000d0ull || e.minY < 0 ||
            e.maxY >= kG570Linear139H)
            return false;

        if (g570Cpu139DeltaOn())
        {
            const uint8_t alphaMode = static_cast<uint8_t>(e.ctx.alpha & 0xffu);
            const GSVertex &c = e.v[2];
            const bool flatUnit = !e.prim.iip && c.r == 1u && c.g == 1u && c.b == 1u &&
                                  c.a == 0x40u;
            if ((alphaMode != 0x62u && alphaMode != 0x68u) ||
                static_cast<uint8_t>((e.ctx.alpha >> 32u) & 0xffu) != 0x80u ||
                !flatUnit || e.ctx.frame.fbmsk != 0u || e.ctx.fba != 0u || e.pabe)
                return false;
        }
    }
    return g404SharedZScope(0x139u, 8u, 0x0d0u, 1u);
}

static void g570Cpu139LinearBegin(uint8_t *vram, size_t vramSize, int y0, int y1)
{
    t_g570Linear139 = {};
    if (vram == nullptr || y0 < 0 || y1 < y0 || y1 >= kG570Linear139H)
        return;

    constexpr uint32_t frameBlock = 0x139u << 5u;
    std::vector<uint32_t> &surface = g570Linear139Surface();
    std::vector<uint8_t> &delta = g570Delta139Surface();
    std::vector<uint8_t> &lastAlpha = g570Delta139Alpha();
    bool deltaEligible = g570Cpu139DeltaOn();
    for (int y = y0; y <= y1; ++y)
    {
        const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
        uint32_t groupBase = 0u;
        for (int x = 0; x < kG570Linear139W; ++x)
        {
            if ((x & 7) == 0)
                groupBase = g530GroupBase(row, static_cast<uint32_t>(x));
            const uint32_t off = groupBase + static_cast<uint32_t>(row.ct[x & 7]) * 4u;
            if (off + 4u > vramSize)
                return;
            std::memcpy(&surface[static_cast<size_t>(y) * kG570Linear139W + x],
                        vram + off, sizeof(uint32_t));
            if (g570Cpu139DeltaOn())
            {
                const size_t i = static_cast<size_t>(y) * kG570Linear139W + x;
                const uint32_t initial = surface[i];
                const uint8_t initialR = static_cast<uint8_t>(initial & 0xffu);
                delta[i] = initialR;
                lastAlpha[i] = 0u;
                deltaEligible = deltaEligible &&
                    initialR == static_cast<uint8_t>((initial >> 8u) & 0xffu) &&
                    initialR == static_cast<uint8_t>((initial >> 16u) & 0xffu);
            }
        }
    }
    t_g570Linear139.active = true;
    t_g570Linear139.delta = deltaEligible;
    t_g570Linear139.y0 = y0;
    t_g570Linear139.y1 = y1;
}

static void g570Cpu139LinearEnd(uint8_t *vram, size_t vramSize)
{
    if (!t_g570Linear139.active || vram == nullptr)
        return;

    constexpr uint32_t frameBlock = 0x139u << 5u;
    uint64_t pixels = 0u;
    uint64_t bad = 0u;
    int firstBadX = -1, firstBadY = -1;
    uint32_t firstCandidate = 0u, firstReference = 0u, firstInitial = 0u;
    std::vector<uint32_t> &surface = g570Linear139Surface();
    std::vector<uint8_t> &delta = g570Delta139Surface();
    std::vector<uint8_t> &lastAlpha = g570Delta139Alpha();
    for (int y = t_g570Linear139.y0; y <= t_g570Linear139.y1; ++y)
    {
        const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
        uint32_t groupBase = 0u;
        for (int x = 0; x < kG570Linear139W; ++x)
        {
            if ((x & 7) == 0)
                groupBase = g530GroupBase(row, static_cast<uint32_t>(x));
            const uint32_t off = groupBase + static_cast<uint32_t>(row.ct[x & 7]) * 4u;
            if (off + 4u > vramSize)
                continue;
            const size_t i = static_cast<size_t>(y) * kG570Linear139W + x;
            uint32_t candidate = surface[i];
            // The clear already committed through the ordinary sprite path.  A delayed shadow
            // reduction must only publish pixels actually hit by a triangle; replay-adjacent GS
            // targets intentionally alias untouched words in the lower part of this allocation.
            if (t_g570Linear139.delta && lastAlpha[i] == 0u)
                continue;
            if (t_g570Linear139.delta)
            {
                const uint8_t level = delta[i];
                candidate = static_cast<uint32_t>(level) |
                            (static_cast<uint32_t>(level) << 8u) |
                            (static_cast<uint32_t>(level) << 16u) |
                            (static_cast<uint32_t>(lastAlpha[i]) << 24u);
            }
            if (g570Cpu139LinearVerifyOn() || g570Cpu139DeltaVerifyOn())
            {
                uint32_t reference = 0u;
                std::memcpy(&reference, vram + off, sizeof(reference));
                if (candidate != reference)
                {
                    if (firstBadX < 0)
                    {
                        firstBadX = x;
                        firstBadY = y;
                        firstCandidate = candidate;
                        firstReference = reference;
                        firstInitial = surface[i];
                    }
                    ++bad;
                }
            }
            else
            {
                std::memcpy(vram + off, &candidate, sizeof(candidate));
            }
            ++pixels;
        }
    }
    t_g570Linear139 = {};

    const uint64_t bands = g_g570Linear139Bands.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t totalPixels =
        g_g570Linear139Pixels.fetch_add(pixels, std::memory_order_relaxed) + pixels;
    const uint64_t totalBad =
        g_g570Linear139Bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bands <= 8u || (bands % 240u) == 0u || bad != 0u)
    {
        std::fprintf(stderr,
                     "[G570:linear139] bands=%llu pixels=%llu bad=%llu mode=%s "
                     "first=(%d,%d init=%08x candidate=%08x reference=%08x)\n",
                     static_cast<unsigned long long>(bands),
                     static_cast<unsigned long long>(totalPixels),
                     static_cast<unsigned long long>(totalBad),
                     (g570Cpu139LinearVerifyOn() || g570Cpu139DeltaVerifyOn())
                         ? (g570Cpu139DeltaVerifyOn() ? "delta-verify" : "linear-verify")
                         : (g570Cpu139DeltaOn() ? "delta-fast" : "linear-fast"),
                     firstBadX, firstBadY, firstInitial, firstCandidate, firstReference);
    }
}

struct G570Cpu139Commit
{
    uint8_t *vram = nullptr;
    size_t vramSize = 0u;
    uint32_t frameBlock = 0u;
    uint32_t frameWidth = 1u;
    uint32_t mask = 0u;
    uint8_t asel = 0u;
    uint8_t bsel = 0u;
    uint8_t csel = 0u;
    uint8_t dsel = 0u;
    uint8_t fix = 0u;
    bool abe = false;
    bool pabe = false;
    bool fba = false;
    int8_t unitDelta = 0;
};

inline G570Cpu139Commit g570BuildCpu139Commit(uint8_t *vram,
                                              size_t vramSize,
                                              uint32_t fbp,
                                              uint32_t fbw,
                                              uint32_t fbmsk,
                                              uint64_t alpha,
                                              uint64_t fba,
                                              bool abe,
                                              bool pabe)
{
    G570Cpu139Commit s;
    s.vram = vram;
    s.vramSize = vramSize;
    s.frameBlock = GSInternal::framePageBaseToBlock(fbp);
    s.frameWidth = (fbw != 0u) ? fbw : 1u;
    s.mask = fbmsk;
    s.asel = static_cast<uint8_t>(alpha & 3u);
    s.bsel = static_cast<uint8_t>((alpha >> 2u) & 3u);
    s.csel = static_cast<uint8_t>((alpha >> 4u) & 3u);
    s.dsel = static_cast<uint8_t>((alpha >> 6u) & 3u);
    s.fix = static_cast<uint8_t>((alpha >> 32u) & 0xFFu);
    s.abe = abe;
    s.pabe = pabe;
    s.fba = (fba & 0x1ull) != 0ull;
    return s;
}

inline int g570PickRgb(uint8_t sel, int src, int dst)
{
    return (sel == 0u) ? src : ((sel == 1u) ? dst : 0);
}

// Build the exact word writePixel() would commit for CT32 + alpha-test-always.  `groupBase` is the
// G530 swizzle expression with its row and 8-pixel-group terms already hoisted.
inline bool g570Cpu139Pixel(const G570Cpu139Commit &s,
                            const G530RowAddr &row,
                            uint32_t groupBase,
                            int x,
                            int y,
                            uint8_t srcR,
                            uint8_t srcG,
                            uint8_t srcB,
                            uint8_t srcA,
                            uint32_t &off,
                            uint32_t &pixel)
{
    off = groupBase + static_cast<uint32_t>(row.ct[x & 0x7]) * 4u;
    if (off + 4u > s.vramSize)
        return false;

    uint32_t existing = 0u;
    if (t_g570Linear139.active)
        existing = g570Linear139Read(x, y);
    else
        std::memcpy(&existing, s.vram + off, sizeof(existing));

    // The combat shadow-volume class is flat (1,1,1,0x40), FIX=0x80 and uses either
    // Cd+Cs or Cd-Cs.  Unsigned packed saturation is exactly the GS scalar result for all three
    // channels, including 0/255, while preserving the original per-fragment read/write order.
    if (s.unitDelta != 0)
    {
        const __m128i dst = _mm_cvtsi32_si128(static_cast<int>(existing));
        const __m128i oneRgb = _mm_cvtsi32_si128(0x00010101);
        const __m128i adjusted = (s.unitDelta > 0)
            ? _mm_adds_epu8(dst, oneRgb)
            : _mm_subs_epu8(dst, oneRgb);
        pixel = (static_cast<uint32_t>(_mm_cvtsi128_si32(adjusted)) & 0x00ffffffu) |
                (static_cast<uint32_t>(srcA) << 24u);
        return true;
    }

    uint8_t r = srcR;
    uint8_t g = srcG;
    uint8_t b = srcB;
    uint8_t a = srcA;
    if (s.abe && !(s.pabe && (a & 0x80u) == 0u))
    {
        const int dr = static_cast<int>(existing & 0xFFu);
        const int dg = static_cast<int>((existing >> 8u) & 0xFFu);
        const int db = static_cast<int>((existing >> 16u) & 0xFFu);
        const int da = static_cast<int>((existing >> 24u) & 0xFFu);
        const int cAlpha = (s.csel == 0u) ? static_cast<int>(a)
                           : (s.csel == 1u) ? da
                                            : static_cast<int>(s.fix);
        r = clampU8(((g570PickRgb(s.asel, r, dr) - g570PickRgb(s.bsel, r, dr)) * cAlpha >> 7) +
                    g570PickRgb(s.dsel, r, dr));
        g = clampU8(((g570PickRgb(s.asel, g, dg) - g570PickRgb(s.bsel, g, dg)) * cAlpha >> 7) +
                    g570PickRgb(s.dsel, g, dg));
        b = clampU8(((g570PickRgb(s.asel, b, db) - g570PickRgb(s.bsel, b, db)) * cAlpha >> 7) +
                    g570PickRgb(s.dsel, b, db));
    }

    if (s.fba)
        a = static_cast<uint8_t>(a | 0x80u);

    pixel = static_cast<uint32_t>(r) |
            (static_cast<uint32_t>(g) << 8u) |
            (static_cast<uint32_t>(b) << 16u) |
            (static_cast<uint32_t>(a) << 24u);
    if (s.mask != 0u)
        pixel = (pixel & ~s.mask) | (existing & s.mask);
    return true;
}

std::atomic<uint64_t> g_g570Cpu139Tris{0u};
std::atomic<uint64_t> g_g570Cpu139Px{0u};
std::atomic<uint64_t> g_g570Cpu139Bad{0u};

inline void g570NoteCpu139(uint64_t px, uint64_t bad)
{
    if (!g570Cpu139StatOn())
        return;
    const uint64_t tris = g_g570Cpu139Tris.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t totalPx = g_g570Cpu139Px.fetch_add(px, std::memory_order_relaxed) + px;
    const uint64_t totalBad = g_g570Cpu139Bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (tris <= 8u || (tris % 20000u) == 0u || bad != 0u)
    {
        std::fprintf(stderr,
                     "[G570:cpu139] tris=%llu px=%llu bad=%llu mode=%s\n",
                     static_cast<unsigned long long>(tris),
                     static_cast<unsigned long long>(totalPx),
                     static_cast<unsigned long long>(totalBad),
                     g570Cpu139VerifyOn() ? "verify" : "fast");
    }
}

} // namespace
