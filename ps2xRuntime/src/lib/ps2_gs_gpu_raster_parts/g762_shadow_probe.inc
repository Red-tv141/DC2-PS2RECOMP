// ===== G762: THE MAP-125 SHADOW — THE PER-CLASS / PER-MARGIN PROBE =====
//
// G761 cornered the defect and could not name it. Its state, in one paragraph: the 0x139 winding
// buffer is produced by the NATIVE GPU BACKEND; the geometry arrives complete (1,800 triangles,
// exactly the hardware dump's count); front and back coverage are exactly balanced (`covAdd ==
// covSub` every frame); the draws reach the GPU unmerged, in order, with the right blend equations;
// the depth attachment is a clean receding ramp with the character embedded; both operands share
// one normalisation; and the consumer's darkening is numerically right (0.63 vs 0.6235). Yet the
// silhouette is torn.
//
// ⭐ THE ARITHMETIC THAT SAYS WHERE TO POINT THIS. `COLCLAMP = 1` saturates the accumulation at 0,
// so the stored winding is always >= max(0, true winding): clamping can only ADD light. A HOLE is
// therefore never an artefact of draw order — at a hole pixel the number of FRONT faces that passed
// GEQUAL is genuinely <= the number of BACK faces that passed. Since the two face sets cover that
// pixel equally often (G761 §3), a hole is produced by the DEPTH COMPARISON alone, on one side or
// the other. This probe reads that comparison directly instead of inferring it from aggregates.
//
// ⛔ Rule G761-1 (four dead probes) says: read the SURFACE where it is computed. Every arm here is
// applied inside `renderBatch`, to the batch's own FBO, and every readback is of that FBO.
//
// FIVE ARMS, ONE BUILD. All default-off; all select the shadow accumulation by SHAPE
// (`fbp == 0x139 && draws >= 100`), never by ordinal — about one 0x139 batch in three is the
// accumulation, the others are a clear and the per-object character composite.
//
//   DC2_G762_DUMP=<n>   dump every n-th selected batch: `g762_<tag>_wind_<k>.pgm` is the RAW
//                       winding (R, not thresholded — G761's dump binarised it and lost the count)
//                       and `g762_<tag>_z_<k>.bin` is the depth attachment as float32 GUEST Z
//                       units (z * 2^32), so margins can be computed offline to the LSB.
//   DC2_G762_NOZ=1      force GL_ALWAYS on the accumulation's draws. The winding must then be
//                       IDENTICALLY ZERO everywhere (the hardware dump replays that way). Anything
//                       else is a coverage defect and moves the whole diagnosis off depth.
//   DC2_G762_ONLY=3|4   execute only the ADD (blend 3) or only the SUB (blend 4) draws. With the
//                       depth test live this is "how many front / back faces passed here", per
//                       pixel; with NOZ it is the raw coverage count. The pair decides WHICH SIDE
//                       of the comparison is wrong — too few front passes, or too many back.
//   DC2_G762_ZBIAS=<f>  add f GUEST Z UNITS to the accumulation's vertex Z before upload (the
//                       clear is left alone). Sweeping it measures the MARGIN distribution
//                       empirically: if the tear closes at +1..+4 the faces are near-coplanar with
//                       the ground and the defect is tie/precision; if it needs hundreds, or never
//                       closes, the volume's Z is wrong wholesale and depth arithmetic is not it.
//   DC2_G762_PIX=x,y    per-draw history of one GS pixel: `(draw index, blend, R after)`. Printed
//                       once at the end of the batch, not per draw, so it cannot interleave.
//
// DC2_G762_TAG=<s> names the dump files so a sweep's arms do not overwrite each other.

static bool g762SelBatch(uint32_t fbp, size_t draws)
{
    return fbp == 0x139u && draws >= 100u;
}

static uint32_t g762DumpEvery()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G762_DUMP");
        if (e == nullptr || e[0] == '\0')
            return 0u;
        const long n = std::strtol(e, nullptr, 10);
        return n > 0 ? static_cast<uint32_t>(n) : 1u;
    }();
    return v;
}

static bool g762NozOn()
{
    static const bool on = std::getenv("DC2_G762_NOZ") != nullptr;
    return on;
}

// 0 = every class, else the only `blend` value whose draws are executed (the clear always runs).
static uint32_t g762OnlyBlend()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G762_ONLY");
        if (e == nullptr || e[0] == '\0')
            return 0u;
        return static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
    }();
    return v;
}

static float g762ZBias()
{
    static const float v = []() -> float {
        const char *e = std::getenv("DC2_G762_ZBIAS");
        if (e == nullptr || e[0] == '\0')
            return 0.0f;
        return static_cast<float>(std::strtod(e, nullptr));
    }();
    return v;
}

// ⛔ REVISION 2, AND THE REASON FOR IT. `DC2_G762_ONLY=4` IS STRUCTURALLY BLIND: the target is
// cleared to 0 and mode 4 is a REVERSE SUBTRACT that clamps at 0, so subtracting the back faces
// from an empty buffer writes nothing at all. The first run measured `sub lit = 0` over the whole
// attachment — not a finding, an arm that cannot produce one. `DC2_G762_SUBADD=1` gives the blend-4
// draws FUNC_ADD instead, so the back-face pass count is readable by exactly the same dump.
static bool g762SubAsAdd()
{
    static const bool on = std::getenv("DC2_G762_SUBADD") != nullptr;
    return on;
}

// ⭐ THE ORACLE. `DC2_G762_GEO=1` writes the selected batch's whole draw list and vertex array
// beside the winding and the depth, so the accumulation can be REPLAYED OFFLINE against the very
// depth field the GPU tested. That splits the last question in two with no further build: if the
// offline replay reproduces the GPU's torn winding, the backend is faithfully executing the data it
// was given and the defect is upstream of it (geometry, or the game's own front/back
// classification); if the replay comes out clean, the tear is made inside the GL rasterisation.
static bool g762GeoOn()
{
    static const bool on = std::getenv("DC2_G762_GEO") != nullptr;
    return on;
}

static const char *g762Tag()
{
    static const char *t = []() -> const char * {
        const char *e = std::getenv("DC2_G762_TAG");
        return (e != nullptr && e[0] != '\0') ? e : "a";
    }();
    return t;
}

// GS-space pixel (top-origin). Returns false when unset.
static bool g762PixSel(int &x, int &y)
{
    static int sx = -1, sy = -1;
    static const bool on = []() -> bool {
        const char *e = std::getenv("DC2_G762_PIX");
        if (e == nullptr || e[0] == '\0')
            return false;
        char *end = nullptr;
        const long a = std::strtol(e, &end, 10);
        if (end == nullptr || *end != ',')
            return false;
        const long b = std::strtol(end + 1, nullptr, 10);
        sx = static_cast<int>(a);
        sy = static_cast<int>(b);
        return true;
    }();
    x = sx;
    y = sy;
    return on;
}

// ⚠️ The ADD and the SUB carry the SAME vertex range shape, so the bias must be applied to the
// accumulation's draws only — biasing the clear sprite as well would move the whole target's Z and
// silently change what the NEXT batch tests against.
static void g762ApplyZBias(G178Batch &b)
{
    const float bias = g762ZBias();
    if (bias == 0.0f || !g762SelBatch(b.fbp, b.draws.size()))
        return;
    static bool s_said = false;
    uint64_t moved = 0ull;
    for (const G178Draw &d : b.draws)
    {
        if (d.blend != 3u && d.blend != 4u)
            continue;
        const size_t lo = static_cast<size_t>(d.firstVtx);
        const size_t hi = lo + static_cast<size_t>(d.vtxCount);
        if (hi > b.verts.size())
            continue;
        for (size_t i = lo; i < hi; ++i)
        {
            b.verts[i].z += bias;
            ++moved;
        }
    }
    if (!s_said)
    {
        s_said = true;
        std::fprintf(stderr, "[G762:arm] zbias=%.3f movedVerts=%llu draws=%zu\n",
                     static_cast<double>(bias), static_cast<unsigned long long>(moved),
                     b.draws.size());
    }
}

static bool g762SkipDraw(const G178Batch &b, const G178Draw &d)
{
    const uint32_t only = g762OnlyBlend();
    if (only == 0u || !g762SelBatch(b.fbp, b.draws.size()))
        return false;
    if (d.blend == 0u)
        return false; // the clear is structural, never an arm
    return static_cast<uint32_t>(d.blend) != only;
}

// ===== the one-pixel history =====
struct G762Pix
{
    std::vector<uint32_t> hist; // (idx<<16) | (blend<<8) | R
    uint64_t batch = 0ull;
    int idx = 0;
    bool active = false;
};
static G762Pix g_g762Pix;

static void g762PixReset(uint64_t batch, uint32_t fbp, size_t draws)
{
    int px = 0, py = 0;
    g_g762Pix.active = g762PixSel(px, py) && g762SelBatch(fbp, draws);
    g_g762Pix.batch = batch;
    g_g762Pix.idx = 0;
    g_g762Pix.hist.clear();
}

static void g762PixAfterDraw(int fbH, uint8_t blend)
{
    if (!g_g762Pix.active)
        return;
    int px = 0, py = 0;
    if (!g762PixSel(px, py))
        return;
    const int glY = fbH - 1 - py;
    const int idx = g_g762Pix.idx++;
    if (glY < 0 || px < 0)
        return;
    uint8_t rgba[4] = {0u, 0u, 0u, 0u};
    glReadPixels(px, glY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    g_g762Pix.hist.push_back((static_cast<uint32_t>(idx & 0xFFFF) << 16) |
                             (static_cast<uint32_t>(blend & 0xFFu) << 8) |
                             static_cast<uint32_t>(rgba[0]));
}

static void g762PixFlush()
{
    if (!g_g762Pix.active || g_g762Pix.hist.empty())
        return;
    int px = 0, py = 0;
    g762PixSel(px, py);
    std::fprintf(stderr, "[G762:pix] batch=%llu gs=(%d,%d) n=%zu ",
                 static_cast<unsigned long long>(g_g762Pix.batch), px, py,
                 g_g762Pix.hist.size());
    // Only transitions are interesting; a run of identical R is collapsed.
    uint32_t prevR = 0xFFFFFFFFu;
    for (const uint32_t e : g_g762Pix.hist)
    {
        const uint32_t r = e & 0xFFu;
        if (r == prevR)
            continue;
        prevR = r;
        std::fprintf(stderr, "%u/%u:%u ", (e >> 16) & 0xFFFFu, (e >> 8) & 0xFFu, r);
    }
    std::fprintf(stderr, "\n");
    g_g762Pix.active = false;
}

// ===== the dump =====
static void g762DumpGeo(uint64_t k, const G178Batch &b)
{
    char path[224];
    std::snprintf(path, sizeof(path), "D:/ps2r/dc2/captures/g762_%s_geo_%05llu.bin", g762Tag(),
                  static_cast<unsigned long long>(k));
    FILE *fp = std::fopen(path, "wb");
    if (fp == nullptr)
        return;
    const uint32_t hdr[5] = {0x32363747u /* 'G762' */, static_cast<uint32_t>(b.fbW),
                             static_cast<uint32_t>(b.fbH),
                             static_cast<uint32_t>(b.draws.size()),
                             static_cast<uint32_t>(b.verts.size())};
    std::fwrite(hdr, sizeof(uint32_t), 5, fp);
    for (const G178Draw &d : b.draws)
    {
        const int32_t rec[10] = {static_cast<int32_t>(d.blend),
                                 static_cast<int32_t>(d.depthFunc & 0x0Fu),
                                 static_cast<int32_t>(d.depthWrite ? 1 : 0),
                                 d.firstVtx, d.vtxCount,
                                 static_cast<int32_t>(d.scX0), static_cast<int32_t>(d.scY0),
                                 static_cast<int32_t>(d.scX1), static_cast<int32_t>(d.scY1),
                                 static_cast<int32_t>(d.texKey != 0u ? 1 : 0)};
        std::fwrite(rec, sizeof(int32_t), 10, fp);
    }
    for (const G178Vtx &v : b.verts)
    {
        const float xyz[3] = {v.x, v.y, v.z};
        std::fwrite(xyz, sizeof(float), 3, fp);
    }
    std::fclose(fp);
    std::fprintf(stderr, "[G762:geo] k=%llu draws=%zu verts=%zu\n",
                 static_cast<unsigned long long>(k), b.draws.size(), b.verts.size());
}

static void g762Dump(uint64_t k, int w, int h, size_t draws)
{
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> rgba(n * 4u);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    std::vector<float> z(n, 0.0f);
    glReadPixels(0, 0, w, h, G761_DEPTH_COMPONENT, GL_FLOAT, z.data());

    std::vector<uint8_t> wind(n);
    std::vector<float> gz(n);
    uint64_t lit = 0ull, hist[9]{};
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t r = rgba[i * 4u + 0u];
        wind[i] = r;
        if (r != 0u)
            ++lit;
        hist[r < 8u ? r : 8u] += 1ull;
        gz[i] = z[i] * 4294967296.0f; // back to GUEST Z units
    }

    char path[224];
    std::snprintf(path, sizeof(path), "D:/ps2r/dc2/captures/g762_%s_wind_%05llu.pgm", g762Tag(),
                  static_cast<unsigned long long>(k));
    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
        std::fwrite(wind.data(), 1u, wind.size(), fp);
        std::fclose(fp);
    }
    std::snprintf(path, sizeof(path), "D:/ps2r/dc2/captures/g762_%s_z_%05llu.bin", g762Tag(),
                  static_cast<unsigned long long>(k));
    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fwrite(gz.data(), sizeof(float), gz.size(), fp);
        std::fclose(fp);
    }
    std::fprintf(stderr,
                 "[G762:dump] k=%llu %dx%d draws=%zu tag=%s noz=%d only=%u subadd=%d zbias=%.3f lit=%llu "
                 "h1=%llu h2=%llu h3=%llu h4=%llu h5=%llu h6=%llu h7=%llu h8+=%llu\n",
                 static_cast<unsigned long long>(k), w, h, draws, g762Tag(),
                 g762NozOn() ? 1 : 0, g762OnlyBlend(), g762SubAsAdd() ? 1 : 0,
                 static_cast<double>(g762ZBias()),
                 static_cast<unsigned long long>(lit),
                 static_cast<unsigned long long>(hist[1]),
                 static_cast<unsigned long long>(hist[2]),
                 static_cast<unsigned long long>(hist[3]),
                 static_cast<unsigned long long>(hist[4]),
                 static_cast<unsigned long long>(hist[5]),
                 static_cast<unsigned long long>(hist[6]),
                 static_cast<unsigned long long>(hist[7]),
                 static_cast<unsigned long long>(hist[8]));
}
