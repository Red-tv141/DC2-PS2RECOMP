// ===== G641: the 0x139 shadow compute's PRODUCER-SCOPED window ==================================
//
// WHY THIS PART EXISTS.
//
// G640 §9 measured the GL worker's whole serial occupancy — the hard floor on the GS pole's blocked
// time — and found **≈77% of it is GPU->CPU readback**. The largest single item is the 0x139 shadow
// compute at ≈4.2 ms/f of `[G447:edge] other`, whose interior (`[G571:gpuwork]`, `DC2_G571_SPLIT=1`)
// is:
//
//     color=0.223 depth=0.157 tri=0.032 span=0.509 offset=0.001 tile=0.010
//     dispatch=0.001 read=2.978  total=3.910 ms
//
// plus, on the GS worker itself (`[G571:batch]`), `read` 0.668 (gather the band out of swizzled
// guest VRAM) and `commit` 0.475 (scatter it back). Every one of those laps moves the WHOLE
// 512x512 surface, and the roadmap's answer was "make the target GPU-resident" — a G636-class
// multi-writer enumeration that has already closed negative twice (G629/G630).
//
// THE OBSERVATION THAT MAKES RESIDENCY UNNECESSARY. Read the compute shader's own body
// (`setupG570Shadow139`, `lle_gpu_raster_backend.inc`):
//
//     uint tile  = uint(ip.y >> 4) * 32u + uint(ip.x >> 4);
//     uint first = offsetWord(tile);
//     uint last  = offsetWord(tile + 1u);
//     uint color = imageLoad(gColor, ip).r;
//     for (uint ref = first; ref < last; ++ref) { ...modify color... }
//     imageStore(gColor, ip, uvec4(color, 0u, 0u, 0u));
//
// For any pixel whose 16x16 tile has `first == last` the loop body NEVER RUNS and the store writes
// back the value the load produced. **The kernel is the identity outside its active tiles.** So for
// those pixels the whole chain — VRAM gather, upload, dispatch, readback, VRAM scatter — is a round
// trip that provably cannot change a byte. That is the producer-to-consumer necessity
// classification the phase asks for, and it is available on the CPU *before* the submit, because
// the tile bins (`g_g570Batch139GpuTileOffsets`) are built by `g570Batch139BuildGpuPayload`.
//
// This is exactly G621's argument (`g621_wrspan.inc`) transplanted from the render-batch colour
// readback onto the compute: intersect what travels with what the PRODUCER can write. G621 narrowed
// rows only; here the producer's claim is a tile mask, so three window shapes are measured and the
// lever takes the one that keeps the transfer a single contiguous rectangle.
//
// ---------------------------------------------------------------------------------------------
// PART 1 — THE CENSUS (`DC2_G641_NEC=1`, default OFF, behaviour-pure).
//
//   [G641:nec] n= tris= tiles=/1024 tileFrac= bboxFrac= rowFrac= x=..  y=..  raggedFrac=
//
//   tiles      active 16x16 tiles out of 1024 (the exact necessity set, at tile granularity)
//   tileFrac   activeTiles/1024 — the floor a per-tile scatter/gather could reach
//   bboxFrac   area of the tile bbox / 262144 — what ONE contiguous glReadPixels sub-rect reaches
//   rowFrac    (tile-row band height * 512) / 262144 — what a contiguous ROW band reaches
//   raggedFrac per-tile-row [txLo..txHi] area / 262144 — what the CPU swizzle loops reach (they are
//              per-row loops already, so raggedness is free there)
//
// Every figure is a WINDOW DELTA against the previous report, never cumulative/frames (the G492
// `fallback` trap). All four are reported side by side so the gap between "one rectangle" and
// "several" is visible rather than assumed (`g495_readback_queue_census.inc`'s rule).
//
// PART 2 — THE LEVER lives in the same file once the census has priced it; until then every entry
// point below is a pure observer and the shipped path is byte-for-byte unchanged.
// ==============================================================================================

namespace
{

// The compute's fixed geometry, mirrored from `g570Batch139BuildGpuPayloadSerial` (16x16 tiles over
// the 512x512 0x139 surface) and from the shader's own `ip.y >> 4` / `* 32u` tile index. A
// static_assert below keeps the mirror honest if kG403DisplayZ* ever moves.
static constexpr int kG641TileSize = 16;
static constexpr int kG641SurfaceW = static_cast<int>(kG403DisplayZW);
static constexpr int kG641SurfaceH = static_cast<int>(kG403DisplayZH);
static constexpr int kG641TileCols = kG641SurfaceW / kG641TileSize;
static constexpr int kG641TileRows = kG641SurfaceH / kG641TileSize;
static constexpr int kG641TileCount = kG641TileCols * kG641TileRows;
static_assert(kG641TileCols == 32 && kG641TileRows == 32,
              "G641 mirrors the shader's 32x32 tile grid; re-derive the shader if this changes");

static bool g641NecCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G641_NEC");
    return on;
}

// ⭐ THE LEVER — PROMOTED DEFAULT-ON by this phase. Rollback `DC2_G641_NO_WIN=1`; the bring-up
// spelling `DC2_G641_WIN=1` stays accepted so the gate's own scripts keep working.
//
// Gates it passed: in-process oracle `bad=0 outside=0` over 2,640 batches / 692,060,160 pixel
// checks; `ridepod` opening pixel bisect p50/p90 = 0.00/0.00 against a control-vs-control floor of
// 0.00/0.76; order-balanced A B B A on `dragon`'s static tail (kicks/f matched to 0.0 across all
// four arms) POOLED **-0.749 ms/f (-2.41%)**, GS own -0.811, both order blocks agreeing in sign.
//
// Restrict every byte that moves in the 0x139 round trip to the producer's own claim:
//   * the guest-VRAM gather and scatter walk only the RAGGED per-tile-row extent (0.304 of the
//     surface on `dragon`) — they are per-row loops already, so raggedness costs nothing;
//   * the GPU upload and readback move the one contiguous tile BBOX (0.516) — a transfer must stay
//     a single rectangle, and the gaps inside it are inactive tiles the kernel cannot change.
//
// The dispatch and the shader are deliberately UNCHANGED (one architectural variable per arm). A
// pixel outside the bbox is therefore still visited by the kernel, still in an inactive tile, still
// the identity, and is never read back — so the stale texture content there can reach nothing.
static bool g641WinOn()
{
    static const bool on = !envFlagEnabled("DC2_G641_NO_WIN");
    return on;
}

// Oracle: run the legacy full-surface chain BESIDE the narrowed one and prove two things per batch —
// (a) every pixel the narrow arm scatters matches the full arm, and (b) every pixel it does NOT
// scatter was unchanged by the full arm. (b) is the actual claim; (a) alone would pass even if the
// window were too small. Strictly more work than either arm, so never valid in a timing run.
static bool g641WinVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G641_VERIFY");
    return on;
}

static bool g641WinStatOn()
{
    static const bool on = envFlagEnabled("DC2_G641_STAT") || g641WinVerifyOn();
    return on;
}

// The producer's claim, recomputed per batch from the tile bins. Owned by the GS worker thread,
// which is the only thread that runs `g570Batch139Prepare` -> the batch site -> the submit.
struct G641Window
{
    bool valid = false;      // false => no active tile at all (the compute is a whole-surface no-op)
    int tileX0 = 0, tileX1 = -1;   // inclusive tile bbox
    int tileY0 = 0, tileY1 = -1;
    int x0 = 0, x1 = -1;     // inclusive pixel bbox (tile bbox * 16)
    int y0 = 0, y1 = -1;
    uint32_t activeTiles = 0u;
    // Per tile-row (32 of them) inclusive active tile-x extent; -1/-2 when the row has none.
    int rowTx0[kG641TileRows]{};
    int rowTx1[kG641TileRows]{};
};

static G641Window g_g641Win;

// Rebuild `g_g641Win` from the payload the front end just built. Returns false when the batch has
// no active tile (which the census counts, and which the lever would turn into a whole no-op).
static bool g641BuildWindow(const std::vector<uint32_t> &tileOffsets, G641Window &w)
{
    w = G641Window();
    for (int i = 0; i < kG641TileRows; ++i)
    {
        w.rowTx0[i] = kG641TileCols;   // empty sentinel: lo > hi
        w.rowTx1[i] = -1;
    }
    if (tileOffsets.size() < static_cast<size_t>(kG641TileCount) + 1u)
        return false;
    int tx0 = kG641TileCols, tx1 = -1, ty0 = kG641TileRows, ty1 = -1;
    uint32_t active = 0u;
    for (int ty = 0; ty < kG641TileRows; ++ty)
    {
        for (int tx = 0; tx < kG641TileCols; ++tx)
        {
            const size_t t = static_cast<size_t>(ty) * kG641TileCols + tx;
            if (tileOffsets[t + 1u] <= tileOffsets[t])
                continue;
            ++active;
            if (tx < w.rowTx0[ty]) w.rowTx0[ty] = tx;
            if (tx > w.rowTx1[ty]) w.rowTx1[ty] = tx;
            if (tx < tx0) tx0 = tx;
            if (tx > tx1) tx1 = tx;
            if (ty < ty0) ty0 = ty;
            if (ty > ty1) ty1 = ty;
        }
    }
    if (active == 0u)
        return false;
    w.valid = true;
    w.activeTiles = active;
    w.tileX0 = tx0; w.tileX1 = tx1;
    w.tileY0 = ty0; w.tileY1 = ty1;
    w.x0 = tx0 * kG641TileSize;
    w.x1 = tx1 * kG641TileSize + (kG641TileSize - 1);
    w.y0 = ty0 * kG641TileSize;
    w.y1 = ty1 * kG641TileSize + (kG641TileSize - 1);
    return true;
}

// --- census accumulators (GS worker thread only) ----------------------------------------------
static uint64_t g_g641NecN = 0ull, g_g641NecEmpty = 0ull;
static uint64_t g_g641NecTris = 0ull, g_g641NecTiles = 0ull;
static uint64_t g_g641NecBboxPx = 0ull, g_g641NecRowPx = 0ull, g_g641NecRaggedPx = 0ull;
static uint64_t g_g641NecX0 = 0ull, g_g641NecX1 = 0ull, g_g641NecY0 = 0ull, g_g641NecY1 = 0ull;
static uint64_t g_g641LastN = 0ull, g_g641LastEmpty = 0ull;
static uint64_t g_g641LastTris = 0ull, g_g641LastTiles = 0ull;
static uint64_t g_g641LastBboxPx = 0ull, g_g641LastRowPx = 0ull, g_g641LastRaggedPx = 0ull;
static uint64_t g_g641LastX0 = 0ull, g_g641LastX1 = 0ull, g_g641LastY0 = 0ull, g_g641LastY1 = 0ull;

static void g641NoteCoverage(size_t triCount, const G641Window &w)
{
    ++g_g641NecN;
    g_g641NecTris += static_cast<uint64_t>(triCount);
    if (!w.valid)
    {
        ++g_g641NecEmpty;
    }
    else
    {
        g_g641NecTiles += w.activeTiles;
        g_g641NecBboxPx += static_cast<uint64_t>(w.x1 - w.x0 + 1) *
                           static_cast<uint64_t>(w.y1 - w.y0 + 1);
        g_g641NecRowPx += static_cast<uint64_t>(w.y1 - w.y0 + 1) *
                          static_cast<uint64_t>(kG641SurfaceW);
        uint64_t ragged = 0ull;
        for (int ty = 0; ty < kG641TileRows; ++ty)
            if (w.rowTx1[ty] >= w.rowTx0[ty])
                ragged += static_cast<uint64_t>(
                              (w.rowTx1[ty] - w.rowTx0[ty] + 1) * kG641TileSize) *
                          static_cast<uint64_t>(kG641TileSize);
        g_g641NecRaggedPx += ragged;
        g_g641NecX0 += static_cast<uint64_t>(w.x0);
        g_g641NecX1 += static_cast<uint64_t>(w.x1);
        g_g641NecY0 += static_cast<uint64_t>(w.y0);
        g_g641NecY1 += static_cast<uint64_t>(w.y1);
    }

    if (g_g641NecN - g_g641LastN < 240ull)
        return;

    const double n = static_cast<double>(g_g641NecN - g_g641LastN);
    const double nz = static_cast<double>((g_g641NecN - g_g641LastN) -
                                          (g_g641NecEmpty - g_g641LastEmpty));
    const double denom = static_cast<double>(kG641SurfaceW) * kG641SurfaceH;
    const double safeNz = nz > 0.0 ? nz : 1.0;
    std::fprintf(stderr,
                 "[G641:nec] n=%.0f empty=%.0f tris=%.1f tiles=%.1f/1024 "
                 "tileFrac=%.3f bboxFrac=%.3f rowFrac=%.3f raggedFrac=%.3f "
                 "x=%.0f..%.0f y=%.0f..%.0f\n",
                 n, static_cast<double>(g_g641NecEmpty - g_g641LastEmpty),
                 static_cast<double>(g_g641NecTris - g_g641LastTris) / n,
                 static_cast<double>(g_g641NecTiles - g_g641LastTiles) / safeNz,
                 static_cast<double>(g_g641NecTiles - g_g641LastTiles) /
                     (safeNz * static_cast<double>(kG641TileCount)),
                 static_cast<double>(g_g641NecBboxPx - g_g641LastBboxPx) / (safeNz * denom),
                 static_cast<double>(g_g641NecRowPx - g_g641LastRowPx) / (safeNz * denom),
                 static_cast<double>(g_g641NecRaggedPx - g_g641LastRaggedPx) / (safeNz * denom),
                 static_cast<double>(g_g641NecX0 - g_g641LastX0) / safeNz,
                 static_cast<double>(g_g641NecX1 - g_g641LastX1) / safeNz,
                 static_cast<double>(g_g641NecY0 - g_g641LastY0) / safeNz,
                 static_cast<double>(g_g641NecY1 - g_g641LastY1) / safeNz);

    g_g641LastN = g_g641NecN;
    g_g641LastEmpty = g_g641NecEmpty;
    g_g641LastTris = g_g641NecTris;
    g_g641LastTiles = g_g641NecTiles;
    g_g641LastBboxPx = g_g641NecBboxPx;
    g_g641LastRowPx = g_g641NecRowPx;
    g_g641LastRaggedPx = g_g641NecRaggedPx;
    g_g641LastX0 = g_g641NecX0;
    g_g641LastX1 = g_g641NecX1;
    g_g641LastY0 = g_g641NecY0;
    g_g641LastY1 = g_g641NecY1;
}

// Called from the batch site once the GPU payload exists. Pure observer while the lever is off.
static void g641NoteBatch()
{
    if (!g641NecCensusOn() && !g641WinOn())
    {
        g_g641Win = G641Window();
        return;
    }
    G641Window w;
    g641BuildWindow(g_g570Batch139GpuTileOffsets, w);
    g_g641Win = w;
    if (g641NecCensusOn())
        g641NoteCoverage(g570Batch139TriCount(), w);
}

// --- the narrowed guest-VRAM gather / scatter --------------------------------------------------
//
// Both mirror `g570Batch139ReadBand` / `g570Batch139WriteBand` exactly — the same
// `g530RowAddr` / `g530GroupBase` swizzle, the same `off + 4 <= vramSize` guard, the same linear
// index — and differ ONLY in which (x, y) they visit. `out` / `in` stay full-surface vectors so
// every existing size check, oracle and index in the chain is untouched.
static void g641ReadRagged(const uint8_t *vram, size_t vramSize, const G641Window &w,
                           std::vector<uint32_t> &out)
{
    constexpr int width = kG641SurfaceW;
    constexpr uint32_t frameBlock = 0x139u << 5u;
    out.assign(static_cast<size_t>(kG641SurfaceH) * width, 0u);
    for (int ty = w.tileY0; ty <= w.tileY1; ++ty)
    {
        if (w.rowTx1[ty] < w.rowTx0[ty])
            continue;
        const int xLo = w.rowTx0[ty] * kG641TileSize;
        const int xHi = w.rowTx1[ty] * kG641TileSize + (kG641TileSize - 1);
        const int yLo = ty * kG641TileSize;
        const int yHi = yLo + (kG641TileSize - 1);
        for (int y = yLo; y <= yHi; ++y)
        {
            const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
            for (int x = xLo; x <= xHi; ++x)
            {
                const uint32_t off = g530GroupBase(row, static_cast<uint32_t>(x)) +
                                     static_cast<uint32_t>(row.ct[x & 7]) * 4u;
                if (off + 4u <= vramSize)
                    std::memcpy(&out[static_cast<size_t>(y) * width + x],
                                vram + off, sizeof(uint32_t));
            }
        }
    }
}

static void g641WriteRagged(uint8_t *vram, size_t vramSize, const G641Window &w,
                            const std::vector<uint32_t> &in)
{
    constexpr int width = kG641SurfaceW;
    constexpr uint32_t frameBlock = 0x139u << 5u;
    if (in.size() < static_cast<size_t>(kG641SurfaceH) * width)
        return;
    for (int ty = w.tileY0; ty <= w.tileY1; ++ty)
    {
        if (w.rowTx1[ty] < w.rowTx0[ty])
            continue;
        const int xLo = w.rowTx0[ty] * kG641TileSize;
        const int xHi = w.rowTx1[ty] * kG641TileSize + (kG641TileSize - 1);
        const int yLo = ty * kG641TileSize;
        const int yHi = yLo + (kG641TileSize - 1);
        for (int y = yLo; y <= yHi; ++y)
        {
            const G530RowAddr row = g530RowAddr(frameBlock, 8u, static_cast<uint32_t>(y));
            for (int x = xLo; x <= xHi; ++x)
            {
                const uint32_t off = g530GroupBase(row, static_cast<uint32_t>(x)) +
                                     static_cast<uint32_t>(row.ct[x & 7]) * 4u;
                if (off + 4u <= vramSize)
                    std::memcpy(vram + off,
                                &in[static_cast<size_t>(y) * width + x], sizeof(uint32_t));
            }
        }
    }
}

// True when the narrowed chain may serve this batch. Fails closed on anything the window cannot
// describe, so the legacy full-surface chain remains the fallback for every refusal.
static bool g641WinUsable()
{
    return g641WinOn() && g_g641Win.valid;
}

static bool g641InRagged(const G641Window &w, int x, int y)
{
    const int ty = y >> 4;
    if (ty < w.tileY0 || ty > w.tileY1 || w.rowTx1[ty] < w.rowTx0[ty])
        return false;
    const int tx = x >> 4;
    return tx >= w.rowTx0[ty] && tx <= w.rowTx1[ty];
}

// --- the three entry points the batch site and the submit use ----------------------------------
//
// `g641ForceFull` exists only for the oracle: it runs the legacy whole-surface chain a second time
// beside the narrowed one, and both go through the same submit.
static thread_local bool t_g641ForceFull = false;

// Fill the rect the submit should carry. Returns true when it narrowed.
static bool g641ShadowRect(int &x, int &y, int &w, int &h)
{
    if (t_g641ForceFull || !g641WinUsable())
        return false;
    const G641Window &win = g_g641Win;
    x = win.x0;
    y = win.y0;
    w = win.x1 - win.x0 + 1;
    h = win.y1 - win.y0 + 1;
    return true;
}

static void g641ReadShadowBand(const uint8_t *vram, size_t vramSize, int y0, int y1,
                               std::vector<uint32_t> &out)
{
    if (!t_g641ForceFull && g641WinUsable())
    {
        g641ReadRagged(vram, vramSize, g_g641Win, out);
        return;
    }
    g570Batch139ReadBand(vram, vramSize, y0, y1, out);
}

static void g641WriteShadowBand(uint8_t *vram, size_t vramSize, int y0, int y1,
                                const std::vector<uint32_t> &in)
{
    if (!t_g641ForceFull && g641WinUsable())
    {
        g641WriteRagged(vram, vramSize, g_g641Win, in);
        return;
    }
    g570Batch139WriteBand(vram, vramSize, y0, y1, in);
}

// --- counters / oracle -------------------------------------------------------------------------
static uint64_t g_g641StatBatches = 0ull, g_g641StatNarrow = 0ull, g_g641StatRefuse = 0ull;
static uint64_t g_g641VfyBatches = 0ull, g_g641VfyChecks = 0ull, g_g641VfyBad = 0ull;
static uint64_t g_g641VfyOutside = 0ull;

static void g641NoteWindowUse(bool narrowed)
{
    if (!g641WinStatOn())
        return;
    ++g_g641StatBatches;
    if (narrowed) ++g_g641StatNarrow; else ++g_g641StatRefuse;
    if (g_g641StatBatches <= 4ull || (g_g641StatBatches % 240ull) == 0ull)
        std::fprintf(stderr,
                     "[G641:win] batches=%llu narrow=%llu refuse=%llu tiles=%u "
                     "bbox=%d..%d/%d..%d\n",
                     static_cast<unsigned long long>(g_g641StatBatches),
                     static_cast<unsigned long long>(g_g641StatNarrow),
                     static_cast<unsigned long long>(g_g641StatRefuse),
                     g_g641Win.activeTiles, g_g641Win.x0, g_g641Win.x1,
                     g_g641Win.y0, g_g641Win.y1);
}

// `narrow` is the candidate surface produced by the narrowed chain (valid inside the bbox only);
// `full` is the legacy whole-surface candidate; `initial` is what guest VRAM held at batch entry.
static void g641NoteVerify(const G641Window &w, const std::vector<uint32_t> &initial,
                           const std::vector<uint32_t> &full,
                           const std::vector<uint32_t> &narrow)
{
    const size_t n = static_cast<size_t>(kG641SurfaceW) * kG641SurfaceH;
    if (initial.size() < n || full.size() < n || narrow.size() < n)
        return;
    ++g_g641VfyBatches;
    uint64_t bad = 0ull, outside = 0ull;
    size_t firstBad = static_cast<size_t>(-1);
    for (int y = 0; y < kG641SurfaceH; ++y)
        for (int x = 0; x < kG641SurfaceW; ++x)
        {
            const size_t i = static_cast<size_t>(y) * kG641SurfaceW + x;
            ++g_g641VfyChecks;
            if (g641InRagged(w, x, y))
            {
                // (a) the narrowed arm must agree with the full arm wherever it publishes.
                if (narrow[i] != full[i])
                {
                    if (firstBad == static_cast<size_t>(-1)) firstBad = i;
                    ++bad;
                }
            }
            else if (full[i] != initial[i])
            {
                // (b) THE CLAIM: outside the published set the kernel changed nothing, so not
                // publishing there loses no byte. A hit here means the window is too small.
                if (firstBad == static_cast<size_t>(-1)) firstBad = i;
                ++bad;
                ++outside;
            }
        }
    g_g641VfyBad += bad;
    g_g641VfyOutside += outside;
    if (bad != 0ull || g_g641VfyBatches <= 4ull || (g_g641VfyBatches % 240ull) == 0ull ||
        outside != 0ull)
        std::fprintf(stderr,
                     "[G641:vfy] batches=%llu checks=%llu bad=%llu outside=%llu"
                     " (this: bad=%llu outside=%llu first=%zd)\n",
                     static_cast<unsigned long long>(g_g641VfyBatches),
                     static_cast<unsigned long long>(g_g641VfyChecks),
                     static_cast<unsigned long long>(g_g641VfyBad),
                     static_cast<unsigned long long>(g_g641VfyOutside),
                     static_cast<unsigned long long>(bad),
                     static_cast<unsigned long long>(outside),
                     static_cast<ptrdiff_t>(firstBad));
}

// Drive the oracle: re-run the LEGACY whole-surface chain from the same `initial` and compare.
// The second submit re-uploads all 512 rows, so it starts from exactly the bytes the shipped path
// would have started from, whatever the narrowed dispatch left in the texture outside its rect.
static void g641RunVerify(const std::vector<uint32_t> &initial,
                          const std::vector<uint32_t> &narrow)
{
    if (!g641WinVerifyOn() || !g_g641Win.valid)
        return;
    std::vector<uint32_t> full;
    t_g641ForceFull = true;
    const bool ok = g570Batch139ReplayGpu(initial, full);
    t_g641ForceFull = false;
    if (!ok)
        return;
    g641NoteVerify(g_g641Win, initial, full, narrow);
}

} // namespace
