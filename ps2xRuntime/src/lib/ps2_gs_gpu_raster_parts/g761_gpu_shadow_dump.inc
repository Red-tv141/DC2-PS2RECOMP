// ===== G761: THE MAP-125 SHADOW, DUMPED WHERE IT IS ACTUALLY COMPUTED =====
// revision: 1
//
// ⚠️ **Rule G761-1 (earned the hard way, four times): an oracle placed in ONE rasterisation leaf
// measures that leaf's admission, not the effect.** The front-end probe went through `drawSprite`
// (the resolve never reaches it), the `drawPrimitive` front door (the depth mirror is not filled
// yet, and that thread runs a frame ahead of the executor), `drawTriangle` (this class never
// reaches it — counted literally zero on two separate arms), and finally guest VRAM (correct for
// COLOUR, but `g403DisplayZBuf()` and the VRAM PSMZ24 surface at `ZBP=0xd0` both read **all zero**,
// because on this route the batch is rendered by the NATIVE GPU BACKEND and its depth lives in a GL
// texture).
//
// So this file dumps the two buffers that decide the shadow, from inside `renderBatch`, right after
// the draw loop, while the batch's own FBO is still bound:
//
//   * the COLOUR attachment — the Z-pass winding buffer the resolve is about to read as PSMCT24;
//   * the DEPTH attachment — the ground depth its GEQUAL test was carved against.
//
// WHAT THE ANSWER LOOKS LIKE. The silhouette's shape is 100 % the depth comparison (proved offline:
// with the depth test removed the 900 fans cancel to winding 0 at every pixel), and our winding
// buffer holds **877 px against the hardware reference's ≈ 2,084**. So either the depth field is
// wrong (partial, stale, or carrying the previous frame's character) — visible directly in
// `g761_gpuz_*.pgm` — or it is right and the volume's own interpolated Z is offset, in which case
// the depth field looks like a clean receding ramp and the tear has no counterpart in it.
//
// Default-off. `DC2_G761_GPU_ZDUMP=<n>` dumps every n-th 0x139 batch to
// `captures/g761_gpuc_<k>.pgm` (colour: winding != 0) and `captures/g761_gpuz_<k>.pgm`
// (depth, normalised over the batch's own min/max so a shallow PS2 Z range is still legible),
// plus a `[G761:gpuz]` line carrying the raw depth range and the winding histogram.

constexpr GLenum G761_DEPTH_COMPONENT = 0x1902;

static uint32_t g761GpuZDumpEvery()
{
    static const uint32_t every = []() -> uint32_t {
        const char *e = std::getenv("DC2_G761_GPU_ZDUMP");
        if (e == nullptr || e[0] == '\0')
            return 0u;
        const long v = std::strtol(e, nullptr, 10);
        return v > 0 ? static_cast<uint32_t>(v) : 1u;
    }();
    return every;
}

// ⭐ The one counter that separates the two remaining explanations for a 0x139 batch carrying
// `draws=13` where the 900 alternating add/sub fans need ~900: either they were admitted and merged
// (by something other than G560 multidraw, which is compiled out), or most were REJECTED at
// `g178ClassifyEntry`'s blend test and only a residue reached the GPU. The per-draw (blend,
// vtxCount) sequence says which, and it is printed beside the batch's own draw count.
static void g761DumpDrawSeq(uint64_t k, const G178Batch &b)
{
    std::fprintf(stderr, "[G761:seq] k=%llu fbp=0x%x draws=%zu seq=",
                 static_cast<unsigned long long>(k), b.fbp, b.draws.size());
    size_t shown = 0;
    for (const G178Draw &d : b.draws)
    {
        if (shown++ >= 48u) { std::fprintf(stderr, " ..."); break; }
        std::fprintf(stderr, " %u/%d", static_cast<unsigned>(d.blend), d.vtxCount);
    }
    std::fprintf(stderr, "\n");
}

static void g761WriteGpuPgm(const char *name, uint64_t k, int w, int h,
                            const std::vector<uint8_t> &px)
{
    char path[192];
    std::snprintf(path, sizeof(path), "D:/ps2r/dc2/captures/g761_%s_%03llu.pgm", name,
                  static_cast<unsigned long long>(k));
    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
        std::fwrite(px.data(), 1u, px.size(), fp);
        std::fclose(fp);
    }
}

// ===== G761 SEAM TRACE =====
// ⭐ §4.3 localised the defect to a **1-2 pixel crack** between the two winding masses, around
// GS (x 204..210, y 344..352) — not a missing mass (the area deficit is ~11 %). Every aggregate in
// this phase is consistent with correctness precisely because a crack pixel is missed by BOTH the
// front and the back face of one seam, so `covAdd == covSub` stays balanced while the winding there
// falls to 0. The disagreement is therefore only visible ONE PIXEL, ONE DRAW AT A TIME.
//
// `DC2_G761_SEAM=1` reads the seam rect back after every draw of an `fbp=0x139` batch and prints a
// line the first time a pixel that WAS nonzero becomes zero, naming the draw index and its blend.
// ⚠️ One glReadPixels per draw over ~360 draws: correctness only, never in a timing run.
static bool g761SeamOn()
{
    static const bool on = std::getenv("DC2_G761_SEAM") != nullptr;
    return on;
}

// GS-space rect (top-origin); converted to GL bottom-origin at read time.
// ⚠️ Revision 1's rect (198..216, 338..358) was too loose: it fired on the first LEGITIMATE
// cancellation anywhere inside it — 333 identical `draw=4 blend=4 ERASED gs=(200,358)` lines, a
// pixel that is inside mass B, not on the seam. A sub is SUPPOSED to cancel an add; what matters
// is which pixels are dark at the END. Revision 2 is scoped to the actual seam between mass A
// (x 206..266, y 332..352) and mass B (x 178..208, y 346..364) and prints the whole rect's
// history, so the accumulation can be read draw by draw instead of guessed from one transition.
constexpr int kG761SeamX0 = 203, kG761SeamX1 = 211;
constexpr int kG761SeamY0 = 343, kG761SeamY1 = 353;

struct G761Seam
{
    std::vector<uint8_t> prev;
    bool have = false;
    int drawIdx = 0;
    uint64_t batch = 0;
    bool reported = false;
};
static G761Seam g_g761Seam;

static void g761SeamReset(uint64_t batch)
{
    g_g761Seam.have = false;
    g_g761Seam.drawIdx = 0;
    g_g761Seam.batch = batch;
    g_g761Seam.reported = false;
}

static void g761SeamAfterDraw(int fbH, uint32_t blend, int vtxCount, int batchDraws)
{
    if (!g761SeamOn())
        return;
    // ⛔ Selecting the batch by ORDINAL picked a 2-draw clear batch: only about one 0x139 batch
    // in three is the shadow accumulation. Select by SHAPE instead - the shadow batch is the only
    // one with hundreds of draws - and trace the Nth such batch (DC2_G761_SEAM_BATCH, default 5) so
    // the route is well past its load transient.
    if (batchDraws < 100)
        return;
    static const uint64_t kWatch = []() -> uint64_t {
        const char *e = std::getenv("DC2_G761_SEAM_BATCH");
        const uint64_t v = (e && e[0]) ? static_cast<uint64_t>(std::strtoull(e, nullptr, 10)) : 0ull;
        return v != 0ull ? v : 5ull;
    }();
    static uint64_t s_seen = 0ull;
    if (g_g761Seam.drawIdx == 0)
        ++s_seen;
    if (s_seen != kWatch)
    {
        ++g_g761Seam.drawIdx;
        return;
    }
    const int w = kG761SeamX1 - kG761SeamX0 + 1;
    const int h = kG761SeamY1 - kG761SeamY0 + 1;
    const int glY = fbH - 1 - kG761SeamY1;
    if (glY < 0 || w <= 0 || h <= 0)
        return;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u);
    glReadPixels(kG761SeamX0, glY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> cur(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < cur.size(); ++i)
        cur[i] = px[i * 4u]; // R == the winding count
    const int idx = g_g761Seam.drawIdx++;
    if (g_g761Seam.have && cur == g_g761Seam.prev)
        return; // this draw did not touch the seam
    // Rows are printed TOP-ORIGIN (GS order) so the layout matches every other dump in this phase.
    std::fprintf(stderr, "[G761:seam] batch=%llu draw=%d blend=%u verts=%d R=",
                 static_cast<unsigned long long>(g_g761Seam.batch), idx,
                 static_cast<unsigned>(blend), vtxCount);
    for (int y = h - 1; y >= 0; --y)
    {
        for (int x = 0; x < w; ++x)
        {
            const uint8_t v = cur[static_cast<size_t>(y) * w + x];
            std::fprintf(stderr, "%c", v == 0u ? '.' : (v < 10u ? char('0' + v) : '+'));
        }
        std::fprintf(stderr, "/");
    }
    std::fprintf(stderr, "\n");
    g_g761Seam.prev.swap(cur);
    g_g761Seam.have = true;
}
