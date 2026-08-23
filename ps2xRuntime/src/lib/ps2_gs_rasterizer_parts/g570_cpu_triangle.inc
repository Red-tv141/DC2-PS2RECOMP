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

// ===== G642: derive the per-row span array IN-SHADER instead of uploading it (default-ON) =====
//
// PREMISE (G641 §8): `gSpans` is the largest remaining CPU->GPU item in the 0x139 chain — 0.51 ms/f
// and ~850 KB/frame. With `DC2_G570_TIGHT_SPAN_139` default-OFF, EVERY per-row span word is
// (xStart = minX, xEnd = maxX) — constant per triangle and already present in header words 0/1 —
// and the three edge values advance linearly in the row index (`rowEdge += rowStep`). So the whole
// array is a function of six extra header words: `rowStep[3]` and `edgeBase[3]`.
//
// ⭐ WHY THE REFUSAL SET DOES NOT MOVE. The shader's edge value is
//     e_k(dx, dy) = edgeBase[k] + rowStep[k]*dy + step[k]*dx
// which is AFFINE in (dx, dy), so over the bbox rectangle [0, dxMax] x [0, dyMax] its extremes are
// at the four corners. The legacy build checks, for every row: `edge_k` (that row's dx=0 corner),
// `advance_k` (= step[k]*dxMax, row-independent) and `edge_k + advance_k` (that row's dx=dxMax
// corner). The union of those over all rows is EXACTLY {four corners fit} + {advance fits}, so the
// O(1) corner check below accepts and refuses precisely the batches the per-row loop did.
//
// ⭐ WHY TRUNCATING `rowStep` TO 32 BITS IS EXACT. The shader accumulates in `uint`, whose
// arithmetic is defined modulo 2^32 (unlike GLSL signed overflow, which is undefined). Reduction
// mod 2^32 is a ring homomorphism, so `uint(edgeBase) + uint(rowStep)*dy + uint(step)*dx` is the
// true value mod 2^32 — and the corner check proves the true value fits int32, so the bitcast back
// is exact. `rowStep` itself therefore needs no `fitsI32` guard (only the corners do), which is
// what keeps the refusal set identical.
//
// Rollback: DC2_G642_NO_DERIVESPAN=1 restores the uploaded per-row array.
inline bool g642DeriveSpanOn()
{
    static const bool on = !envFlagEnabled("DC2_G642_NO_DERIVESPAN");
    return on;
}

// The per-triangle header stride, shared by both payload builders and the compute shader. G642
// grew it 20 -> 26: words 20..22 are `rowStep[0..2]`, words 23..25 are `edgeBase[0..2]`.
//
// ===== ⭐ G649 (ROADMAP P3): the header re-laid out HOT-FIRST and QUAD-ALIGNED, 26 -> 28 =========
//
// PREMISE: `[G641:sh] comp` is 1.12–1.13 ms/f after G643's SSBO move and is the largest single item
// inside the GL worker after render issue. The kernel is a per-PIXEL walk of the tile's triangle
// list, and the overwhelming majority of the refs it visits are REJECTED — by the row test, by the
// column test, or by the edge test — before a single float is read.
//
// ⛔ ROADMAP P3's first proposal ("sort each tile's triangle list so the header fetches are
// sequential") is a NO-OP on the shipped binary: the bin IS already a counting sort in ascending
// triangle index, which is exactly why G639's parallel build could reproduce it byte for byte. The
// walk is already monotonic. Do not re-attempt it.
//
// What IS true is P3's second proposal — the READ ORDER, i.e. the layout. Under the G642 layout a
// pixel that rejects still straddles the whole 104-byte record, because its reject fields are words
// 0..3, 5..8 AND 20..25. This layout puts all fourteen reject-stage words in words 0..15 and every
// float that is only read PAST the edge test in words 16..27, and pads the stride to 28 = seven
// `uvec4`s so the SSBO can be declared as a `uvec4[]` and each stage costs ONE vector load instead
// of two to nine scalar ones:
//     quad 0  minX maxX minY maxY          <- the row+column reject, one load
//     quad 1  step0 step1 step2 mode       <- the edge test
//     quad 2  rowStep0..2 spanOffset
//     quad 3  edgeBase0..2 (pad)
//     quad 4  a0 b0 a1 b1                  <- cold: only past the edge test
//     quad 5  fx2 fy2 winding invDenom     <- cold
//     quad 6  z0 z1 z2 (pad)               <- cold
// The upload grows 7.7% (two words per triangle); the reject path's fetch drops from up to nine
// scalar loads spread over two cache lines to one 16-byte load.
//
// ⚠️ `precise float` stays exactly as it was — this changes WHERE the words live, never how they
// are computed. The G642 oracle and `DC2_G570_VERIFY_GPU_139` both still apply unchanged.
constexpr size_t kG642TriStride = 28u;

// G649: named slots, so the two CPU builders, the G642 oracle and the shader cannot drift apart.
enum : size_t
{
    kG649HMinX = 0u, kG649HMaxX = 1u, kG649HMinY = 2u, kG649HMaxY = 3u,
    kG649HStep0 = 4u, kG649HStep1 = 5u, kG649HStep2 = 6u, kG649HMode = 7u,
    kG649HRowStep0 = 8u, kG649HRowStep1 = 9u, kG649HRowStep2 = 10u, kG649HSpanOff = 11u,
    kG649HEdge0 = 12u, kG649HEdge1 = 13u, kG649HEdge2 = 14u, kG649HPad0 = 15u,
    kG649HA0 = 16u, kG649HB0 = 17u, kG649HA1 = 18u, kG649HB1 = 19u,
    kG649HFx2 = 20u, kG649HFy2 = 21u, kG649HWinding = 22u, kG649HInvDenom = 23u,
    kG649HZ0 = 24u, kG649HZ1 = 25u, kG649HZ2 = 26u, kG649HPad1 = 27u,
};

// The derivation is only the identity when the spans are the full bbox, i.e. when G571's tight-span
// slice is NOT armed. Tight spans keep the uploaded table.
inline bool g642DeriveActive()
{
    return g642DeriveSpanOn() && !g570Batch139TightGpuSpansOn();
}

// ===== G648: stop BUILDING the CPU span array too (ROADMAP Priority 6) =========================
//
// G642 deleted the span array's UPLOAD but the CPU still built it, because the CPU band replay
// reads it when the GPU path refuses. `[G638:prep]` re-taken after G642/G643 shows why that
// matters: `g570prep`'s per-triangle setup loop `^tris` is now **19.83% of the whole CPU-fallback
// dispatch window at 103.5 us/batch**, and G642's own census proves the loop writes
// **5.35 MB/batch** into `g_g570Batch139Spans` (343 k rows x 4 bytes). Meanwhile
// `[G570:gpu139] failed=0` — on this route the CPU fallback never runs.
//
// ⭐ THE SPLIT. This is the CPU/GPU workload division stated as a lever: the CPU currently does
// 100% of the preparation for BOTH paths regardless of which one serves the batch. In the derive
// configuration every row's span is (xStart = minX, xEnd = maxX), which are ALREADY fields of the
// `G570Batch139Tri` the loop just built — so the array is pure redundancy. `g648SpanAt()` reads
// those two fields instead, and the array is simply never materialised.
//
// ⭐ WHY THIS NEEDS NO "MATERIALISE BEFORE USE" PROTOCOL. The accessor is the only reader, it is
// branch-free on a process-lifetime bool, and it is exact on BOTH arms — so the CPU band replay is
// byte-identical whether or not the array exists, and no fork point has to be found or ordered.
// (An enumerate-the-consumers-and-materialise design would have had exactly the failure mode
// G636 documents: one missed consumer, silent corruption.)
//
// Rollback: DC2_G648_NO_LAZYSPAN=1. Automatically disabled under DC2_G642_VERIFY=1, whose oracle
// compares the derivation AGAINST the eagerly built array.
inline bool g648LazySpanOn()
{
    static const bool on = !envFlagEnabled("DC2_G648_NO_LAZYSPAN");
    return on;
}

// Set once per batch by g570Batch139PrepareBuild; read by g648SpanAt from the GS thread and from
// every replay lane. Written before any lane starts and never during a dispatch.
// (The accessor itself lives below, next to the arrays it reads.)
static bool g_g648SpansDeferred = false;

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

// ===== ⭐ G649 (ROADMAP P7): the CPU-replay half is no longer STORED ===========================
//
// G648 established the principle — the CPU should not prepare what the GPU path will not read — and
// applied it to the span array. The same argument applies to this struct. Seven of its members were
// written by the setup loop on the GS pole thread purely so a CPU band replay could read them back:
//     fx[3] fy[3] sign            84 bytes ... the SCALED integer vertices and the winding sign
//     rawX[3] rawY[3] ofx ofy         ... the unscaled vertices and the XY offset
// `[G570:gpu139] failed=0` on `dragon`: that replay never runs there.
//
// ⭐ Neither half needs a "materialise before use" protocol, because BOTH are recoverable EXACTLY:
//
//   1. `fx/fy/sign` only ever appear inside the edge functions, and G642 already proved that the
//      edge function is AFFINE in (column, row):
//          e_k(x,y) = edgeBase[k] + rowStep[k]*(y - minY) + step[k]*(x - minX)
//      Substituting the definitions of `edgeBase`, `rowStep` and `step` turns
//          ((fx[2]-fx[1])*(y*16 - fy[1]) - (fy[2]-fy[1])*(x*16 - fx[1])) * sign
//      into exactly that expression — an INTEGER identity in int64, with intermediates no larger
//      than the ones it replaces. So every consumer can read the three header fields it already has
//      instead of the six it no longer does.
//
//   2. `rawX/rawY/ofx/ofy` are literal COPIES of `g_g144List[i+1].v[k].x/.y` and
//      `ctx.xyoffset.ofx >> 4`. No arithmetic, so re-reading them from the entry list is bit-exact
//      by construction — no MXCSR question, unlike the float setup the replay already recomputes
//      per lane on purpose.
//
// The struct drops 232 -> 148 bytes, so the setup loop's stores, the `resize` that precedes them and
// every later walk of the array all shrink by ~36%.
//
// ⚠️ ROLLBACK IS A BINARY, NOT A FLAG. A struct member cannot be removed under an env test without
// keeping both forms live, which is exactly the shape G645 refuted. This lever is therefore gated
// the way G602 gated its compile-time lever: an order-balanced A B B A across two saved executables
// via `tools/g525_route.ps1 -Exe`. The control is `dc2_runner_g649p23.exe` (P2 + P3 + P4 + P6, no
// P7), kept beside the shipped runner.
struct G570Batch139Tri
{
    int minX = 0, maxX = -1, minY = 0, maxY = -1;
    int64_t step[3]{};
    int64_t rowStep[3]{};
    int64_t edgeBase[3]{};
    uint8_t topLeftMask = 0u;
    int8_t delta = 0;
    uint32_t spanOffset = 0u;
    float a0 = 0.0f, b0 = 0.0f, a1 = 0.0f, b1 = 0.0f;
    float fx2 = 0.0f, fy2 = 0.0f;
    float winding = 1.0f, invAbsDenom = 0.0f;
    float z0 = 0.0f, z1 = 0.0f, z2 = 0.0f;
};

// G649: the three edge functions at an arbitrary pixel, from the header fields alone. Identical in
// int64 to the `fx/fy/sign` expression it replaces — see the derivation above.
static inline void g649EdgesAt(const G570Batch139Tri &p, int x, int y, int64_t e[3])
{
    const int64_t dx = static_cast<int64_t>(x) - p.minX;
    const int64_t dy = static_cast<int64_t>(y) - p.minY;
    e[0] = p.edgeBase[0] + p.rowStep[0] * dy + p.step[0] * dx;
    e[1] = p.edgeBase[1] + p.rowStep[1] * dy + p.step[1] * dx;
    e[2] = p.edgeBase[2] + p.rowStep[2] * dy + p.step[2] * dx;
}

// G649: the CPU replay's local-space vertices, straight from the entry the triangle was built from.
// Triangle `i` is `g_g144List[i + 1]` in BOTH arms of the setup loop, and each field is a literal
// copy — no arithmetic, so this cannot differ from what the removed members held.
struct G649TriRaw
{
    float lx[3];
    float ly[3];
};
static inline void g649TriRawAt(size_t triIndex, G649TriRaw &out)
{
    const G144Entry &e = g_g144List[triIndex + 1u];
    const float ofx = static_cast<float>(e.ctx.xyoffset.ofx >> 4);
    const float ofy = static_cast<float>(e.ctx.xyoffset.ofy >> 4);
    out.lx[0] = e.v[0].x - ofx; out.ly[0] = e.v[0].y - ofy;
    out.lx[1] = e.v[1].x - ofx; out.ly[1] = e.v[1].y - ofy;
    out.lx[2] = e.v[2].x - ofx; out.ly[2] = e.v[2].y - ofy;
}

std::vector<G570Batch139Tri> g_g570Batch139Tris;
std::vector<uint32_t> g_g570Batch139Spans;
std::vector<uint32_t> g_g570Batch139GpuTris;
std::vector<uint32_t> g_g570Batch139GpuSpans;
std::vector<uint32_t> g_g570Batch139GpuTileOffsets;
std::vector<uint32_t> g_g570Batch139GpuTileTris;
std::vector<uint32_t> g_g570Batch139GpuDebug;
std::vector<uint32_t> g_g570Batch139CpuDebug;
int g_g570Batch139RejectReason = 1;

// G648: the one reader of a per-row span. Exact on BOTH arms — in the deferred configuration every
// row's span IS the triangle's bbox, which `p` already carries. See the note beside g648LazySpanOn.
static inline uint32_t g648SpanAt(const G570Batch139Tri &p, int y)
{
    if (g_g648SpansDeferred)
        return static_cast<uint32_t>(p.minX & 0xffff) |
               (static_cast<uint32_t>(p.maxX & 0xffff) << 16u);
    return g_g570Batch139Spans[static_cast<size_t>(p.spanOffset) +
                               static_cast<size_t>(y - p.minY)];
}

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

// ⚠️ STALE DIAGNOSTIC, currently unreferenced: its `* 23u` / `* 7u` strides predate the 20-word
// header the payload builders have emitted since G571 (and the 26-word one since G642). It also
// indexes `g_g570Batch139Spans` directly, which G648 may leave unbuilt — hence the early return.
// Fix its strides before wiring it to any caller.
static uint32_t g570Batch139PackedCoverageHash(int x, int y)
{
    if (g_g648SpansDeferred)
        return 0u;
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
                int64_t edge[3];
                g649EdgesAt(p, x, y, edge); // G649: same value, from the header fields
                const int64_t edge0 = edge[0], edge1 = edge[1], edge2 = edge[2];
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

// ===== G639: the 0x139 GPU payload build, PARALLEL (bring-up DEFAULT OFF, DC2_G639_PAYPAR=1) =====
//
// PREMISE (G638 §2.2, measured on `dragon`'s static tail): `g570Batch139Prepare` is 4.735 ms/f of a
// 33.8 ms/f **sole GS pole**, and `[G638:prep] ^payload` puts **76% of it (≈3.6 ms/f)** inside
// `g570Batch139BuildGpuPayloadSerial` below — one thread, on the pole, once per frame.
//
// ⭐ WHY IT PARALLELISES EXACTLY. Every output offset is known BEFORE any word is written:
//   * the tri header is a fixed 20-word stride, so triangle i owns `[i*20, i*20+20)`;
//   * a triangle emits one 4-word span record per bbox ROW, and `maxY-minY+1` is already in the
//     `G570Batch139Tri`, so a serial O(n) prefix pass fixes every triangle's span base — and that
//     prefix IS the `gpuSpanOffset` the serial build writes into word 4 (it pushes
//     `GpuSpans.size()/4` at that moment);
//   * the tile bin is the only order-sensitive output, and it is a counting sort. Counting it
//     PER CHUNK and prefix-summing **tile-major, chunk-minor** puts chunk c's triangles for tile t
//     after chunk c-1's, and each chunk fills its own region in increasing triangle index — so the
//     bin list is byte-identical to the serial cursor walk.
// Every participant therefore writes a disjoint range. Nothing is reordered, nothing is recomputed,
// no arithmetic changes: this is the same law G637 §3 used for the band replay (contiguous /
// disjoint / in-order), applied to a build instead of a rasterization.
//
// ⭐ CHUNKING. Equal triangle COUNTS are not equal work — a triangle's cost is its bbox row count.
// Per G637 §5 the answer is NOT a work estimator: over-decompose (4 chunks per lane) and let
// `GSRowPool::runCoop` steal, with the caller participating. Same engine G638 promoted.
//
// ⛔ The serial build returns false on the first `fitsI32` overflow, leaving the vectors partly
// written; the caller then sets reject reason 7 and the next `Prepare` clears them. The parallel
// build sets an atomic flag and returns false after the join, so it may do more work before
// bailing — the vectors are discarded either way, so the observable behaviour is identical.
//
// Oracle: DC2_G639_PAYPAR_VERIFY=1 runs BOTH and compares all four output vectors byte for byte.
static bool g570Batch139BuildGpuPayloadSerial();

// ⭐⭐⭐ PROMOTED DEFAULT-ON (G639). Exactness: `DC2_G639_PAYPAR_VERIFY=1` built every batch BOTH
// ways and compared all four output vectors byte for byte — **bad=0 on 2,400 batches** (~10.9 M
// triangles), zero MISMATCH lines. Payoff: order-balanced A B B A on `dragon`'s static tail,
// 2,040 presents matched per arm, both order blocks agreeing in sign:
//     block 1 (A1 vs B1): frame -2.333 ms/f (-6.84%)   GS own -2.344
//     block 2 (A2 vs B2): frame -3.430 ms/f (-9.57%)   GS own -3.309
//     POOLED: 34.975 -> 32.094 = -2.882 ms/f (-8.24%)  GS own 32.857 -> 30.030
// Rollback: DC2_G639_NO_PAYPAR=1 -> g570Batch139BuildGpuPayloadSerial, which is the original
// function renamed and otherwise byte-for-byte unchanged.
static bool g639PayParOn()
{
    static const bool on = !envFlagEnabled("DC2_G639_NO_PAYPAR");
    return on;
}

static bool g639PayParVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G639_PAYPAR_VERIFY");
    return on;
}

static bool g639PayParStatOn()
{
    static const bool on = envFlagEnabled("DC2_G639_PAYPAR_STAT") || g639PayParVerifyOn();
    return on;
}

// Scratch, reused across frames (never reallocated in the steady state).
static std::vector<uint32_t> g_g639SpanBase;        // per triangle, in 4-word span records
static std::vector<uint32_t> g_g639ChunkTileCount;  // chunks * tileCount
static std::vector<uint32_t> g_g639ChunkTileCursor; // chunks * tileCount
static std::vector<int> g_g639Bounds;               // chunks+1 triangle fences
static std::atomic<uint32_t> g_g639Fail{0u};
static uint64_t g_g639ParBatches = 0ull, g_g639ParBad = 0ull;

static bool g570Batch139BuildGpuPayloadParallel()
{
    constexpr int tileShift = 4;
    constexpr int tileSize = 1 << tileShift;
    constexpr int tilesX = static_cast<int>(kG403DisplayZW) / tileSize;
    constexpr int tilesY = static_cast<int>(kG403DisplayZH) / tileSize;
    constexpr size_t tileCount = static_cast<size_t>(tilesX) * tilesY;
    constexpr size_t triStride = kG642TriStride;

    const size_t n = g_g570Batch139Tris.size();
    g_g570Batch139GpuTris.clear();
    g_g570Batch139GpuSpans.clear();
    g_g570Batch139GpuTileOffsets.clear();
    g_g570Batch139GpuTileTris.clear();
    if (n == 0u)
        return false; // unreachable: the caller rejects an empty tri list first (reason 6)

    const bool tileCull = g570Batch139TileCullOn();
    const auto fitsI32 = [](int64_t value) {
        return value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
    };

    // ---- Phase 1 (serial, O(n)): fix every output offset up front.
    // G642: in derive mode there ARE no span records — the array collapses to a single padded row
    // that exists only to keep image unit 3 bound with something valid.
    const bool derive = g642DeriveActive();
    g_g639SpanBase.resize(n);
    uint32_t spanRecs = 0u;
    if (derive)
        std::fill(g_g639SpanBase.begin(), g_g639SpanBase.end(), 0u);
    else
        for (size_t i = 0; i < n; ++i)
        {
            g_g639SpanBase[i] = spanRecs;
            const G570Batch139Tri &p = g_g570Batch139Tris[i];
            if (p.maxX >= p.minX)
                spanRecs += static_cast<uint32_t>(p.maxY - p.minY + 1);
        }
    g_g570Batch139GpuTris.resize(n * triStride);
    g_g570Batch139GpuSpans.resize(derive ? 1u : static_cast<size_t>(spanRecs) * 4u, 0u);

    // ---- Chunking: over-decompose 4x the lane count, exactly as G637's replay does.
    int lanes = (g_g144Threads > 1) ? g_g144Threads : 1;
    const int cap = GSRowPool::instance().laneCap();
    if (lanes > cap)
        lanes = cap;
    int chunks = lanes * 4;
    if (chunks > static_cast<int>(n))
        chunks = static_cast<int>(n);
    if (chunks < 1)
        chunks = 1;
    g_g639Bounds.resize(static_cast<size_t>(chunks) + 1u);
    for (int c = 0; c <= chunks; ++c)
        g_g639Bounds[static_cast<size_t>(c)] =
            static_cast<int>(static_cast<int64_t>(n) * c / chunks);
    g_g639ChunkTileCount.assign(static_cast<size_t>(chunks) * tileCount, 0u);
    g_g639Fail.store(0u, std::memory_order_relaxed);

    const int *bounds = g_g639Bounds.data();
    const auto chunkOf = [bounds, chunks](int a) {
        int lo = 0, hi = chunks - 1;
        while (lo < hi)
        {
            const int mid = (lo + hi + 1) >> 1;
            if (bounds[mid] <= a) lo = mid; else hi = mid - 1;
        }
        return lo;
    };

    // ---- Phase 2 (parallel): headers + span records + per-chunk tile counts.
    const std::function<void(int, int)> emit = [&](int a, int b) {
        uint32_t *cnt = g_g639ChunkTileCount.data() +
                        static_cast<size_t>(chunkOf(a)) * tileCount;
        for (int i = a; i <= b; ++i)
        {
            const G570Batch139Tri &p = g_g570Batch139Tris[static_cast<size_t>(i)];
            const uint32_t gpuSpanOffset = g_g639SpanBase[static_cast<size_t>(i)];
            if (!fitsI32(p.step[0]) || !fitsI32(p.step[1]) || !fitsI32(p.step[2]))
            {
                g_g639Fail.store(1u, std::memory_order_relaxed);
                return;
            }
            uint32_t *h = g_g570Batch139GpuTris.data() + static_cast<size_t>(i) * triStride;
            // G649: hot-first, quad-aligned (see the layout note beside kG642TriStride).
            h[kG649HMinX] = static_cast<uint32_t>(p.minX);
            h[kG649HMaxX] = static_cast<uint32_t>(p.maxX);
            h[kG649HMinY] = static_cast<uint32_t>(p.minY);
            h[kG649HMaxY] = static_cast<uint32_t>(p.maxY);
            h[kG649HStep0] = static_cast<uint32_t>(p.step[0]);
            h[kG649HStep1] = static_cast<uint32_t>(p.step[1]);
            h[kG649HStep2] = static_cast<uint32_t>(p.step[2]);
            h[kG649HMode] = static_cast<uint32_t>(p.topLeftMask) | (p.delta > 0 ? 0x100u : 0u);
            // G642: the six words that replace the whole per-row array. Truncation to 32 bits is
            // exact under the shader's `uint` accumulation (see the note beside g642DeriveSpanOn).
            h[kG649HRowStep0] = static_cast<uint32_t>(p.rowStep[0]);
            h[kG649HRowStep1] = static_cast<uint32_t>(p.rowStep[1]);
            h[kG649HRowStep2] = static_cast<uint32_t>(p.rowStep[2]);
            h[kG649HSpanOff] = gpuSpanOffset;
            h[kG649HEdge0] = static_cast<uint32_t>(p.edgeBase[0]);
            h[kG649HEdge1] = static_cast<uint32_t>(p.edgeBase[1]);
            h[kG649HEdge2] = static_cast<uint32_t>(p.edgeBase[2]);
            h[kG649HPad0] = 0u;
            h[kG649HA0] = g570Batch139FloatBits(p.a0);
            h[kG649HB0] = g570Batch139FloatBits(p.b0);
            h[kG649HA1] = g570Batch139FloatBits(p.a1);
            h[kG649HB1] = g570Batch139FloatBits(p.b1);
            h[kG649HFx2] = g570Batch139FloatBits(p.fx2);
            h[kG649HFy2] = g570Batch139FloatBits(p.fy2);
            h[kG649HWinding] = g570Batch139FloatBits(p.winding);
            h[kG649HInvDenom] = g570Batch139FloatBits(p.invAbsDenom);
            h[kG649HZ0] = g570Batch139FloatBits(p.z0);
            h[kG649HZ1] = g570Batch139FloatBits(p.z1);
            h[kG649HZ2] = g570Batch139FloatBits(p.z2);
            h[kG649HPad1] = 0u;
            if (p.maxX < p.minX)
                continue;

            // G642: the O(1) four-corner guard. Exactly equivalent to the union of the legacy
            // per-row checks below, so an admitted batch stays admitted and a refused one stays
            // refused. Skipped when the row range is empty, because the per-row loop it replaces
            // would then run zero iterations and check nothing.
            if (derive)
            {
                if (p.maxY >= p.minY)
                {
                    const int64_t dxMax =
                        std::max<int64_t>(0, static_cast<int64_t>(p.maxX) - p.minX);
                    const int64_t dyMax = static_cast<int64_t>(p.maxY) - p.minY;
                    for (int k = 0; k < 3; ++k)
                    {
                        const int64_t eTop = p.edgeBase[k];
                        const int64_t eBot = p.edgeBase[k] + p.rowStep[k] * dyMax;
                        const int64_t adv = p.step[k] * dxMax;
                        if (!fitsI32(eTop) || !fitsI32(eBot) || !fitsI32(adv) ||
                            !fitsI32(eTop + adv) || !fitsI32(eBot + adv))
                        {
                            g_g639Fail.store(1u, std::memory_order_relaxed);
                            return;
                        }
                    }
                }
            }
            else
            {

            // G649 (P7): these ARE `p.edgeBase` and `p.rowStep` — the setup loop computed them from
            // the same `fx/fy/sign` at the same `(minX*16, minY*16)` base.
            int64_t rowEdge[3] = {
                p.edgeBase[0], p.edgeBase[1], p.edgeBase[2],
            };
            const int64_t rowStep[3] = {
                p.rowStep[0], p.rowStep[1], p.rowStep[2],
            };
            uint32_t *sp = g_g570Batch139GpuSpans.data() +
                           static_cast<size_t>(gpuSpanOffset) * 4u;
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
                const int64_t dx = std::max<int64_t>(0, xEnd - xStart);
                const int64_t advance0 = p.step[0] * dx;
                const int64_t advance1 = p.step[1] * dx;
                const int64_t advance2 = p.step[2] * dx;
                if (!fitsI32(edge0) || !fitsI32(edge1) || !fitsI32(edge2) ||
                    !fitsI32(advance0) || !fitsI32(advance1) || !fitsI32(advance2) ||
                    !fitsI32(edge0 + advance0) || !fitsI32(edge1 + advance1) ||
                    !fitsI32(edge2 + advance2))
                {
                    g_g639Fail.store(1u, std::memory_order_relaxed);
                    return;
                }
                sp[0] = packedSpan;
                sp[1] = static_cast<uint32_t>(edge0);
                sp[2] = static_cast<uint32_t>(edge1);
                sp[3] = static_cast<uint32_t>(edge2);
                sp += 4;
                rowEdge[0] += rowStep[0];
                rowEdge[1] += rowStep[1];
                rowEdge[2] += rowStep[2];
            }
            } // G642: end of the legacy per-row span build

            const int tx0 = p.minX >> tileShift;
            const int tx1 = p.maxX >> tileShift;
            const int ty0 = p.minY >> tileShift;
            const int ty1 = p.maxY >> tileShift;
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx)
                    if (!tileCull || g570Batch139TileMayCover(p, tx, ty))
                        ++cnt[static_cast<size_t>(ty) * tilesX + tx];
        }
    };
    GSRowPool::instance().runCoop(bounds, chunks, lanes - 1, emit);
    if (g_g639Fail.load(std::memory_order_relaxed) != 0u)
        return false;

    // ---- Phase 3 (serial, O(tiles * chunks) = 896 * 32): the counting-sort prefix.
    g_g570Batch139GpuTileOffsets.assign(tileCount + 1u, 0u);
    g_g639ChunkTileCursor.resize(static_cast<size_t>(chunks) * tileCount);
    uint32_t running = 0u;
    for (size_t t = 0; t < tileCount; ++t)
    {
        g_g570Batch139GpuTileOffsets[t] = running;
        for (int c = 0; c < chunks; ++c)
        {
            const size_t k = static_cast<size_t>(c) * tileCount + t;
            g_g639ChunkTileCursor[k] = running;
            running += g_g639ChunkTileCount[k];
        }
    }
    g_g570Batch139GpuTileOffsets[tileCount] = running;
    g_g570Batch139GpuTileTris.resize(running);

    // ---- Phase 4 (parallel): fill each chunk's own slice of every tile, in triangle order.
    const std::function<void(int, int)> bin = [&](int a, int b) {
        uint32_t *cur = g_g639ChunkTileCursor.data() +
                        static_cast<size_t>(chunkOf(a)) * tileCount;
        for (int i = a; i <= b; ++i)
        {
            const G570Batch139Tri &p = g_g570Batch139Tris[static_cast<size_t>(i)];
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
                    g_g570Batch139GpuTileTris[cur[tile]++] = static_cast<uint32_t>(i);
                }
        }
    };
    GSRowPool::instance().runCoop(bounds, chunks, lanes - 1, bin);

    if (g_g570Batch139GpuTris.empty() || g_g570Batch139GpuSpans.empty() ||
        g_g570Batch139GpuTileTris.empty())
        return false;
    g570Batch139PadGpuWords(g_g570Batch139GpuTris);
    g570Batch139PadGpuWords(g_g570Batch139GpuSpans);
    g570Batch139PadGpuWords(g_g570Batch139GpuTileOffsets);
    g570Batch139PadGpuWords(g_g570Batch139GpuTileTris);
    return true;
}

// ===== G642 oracle & census =====
// The derivation rests on exactly two claims about the legacy payload, and this checks BOTH against
// the legacy construction on the same batch:
//   (a) every row's span is exactly the triangle bbox (xStart == minX, xEnd == maxX), and
//   (b) the row's three edge values equal `edgeBase[k] + rowStep[k]*dy` in 32-bit two's complement.
// The per-pixel `+ step[k]*dx` term is character-for-character the same expression in both shader
// arms, so (a) and (b) close the argument. Strictly more work than either arm; never valid in a
// timing run.
static bool g642VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G642_VERIFY");
    return on;
}

static bool g642StatOn()
{
    static const bool on = envFlagEnabled("DC2_G642_STAT") || g642VerifyOn();
    return on;
}

static uint64_t g_g642Batches = 0ull, g_g642Rows = 0ull, g_g642Bad = 0ull;
static uint64_t g_g642SavedWords = 0ull;

static void g642VerifyBatch()
{
    const size_t n = g_g570Batch139Tris.size();
    for (size_t i = 0; i < n; ++i)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[i];
        if (p.maxX < p.minX || p.maxY < p.minY)
            continue;
        if ((i + 1u) * kG642TriStride > g_g570Batch139GpuTris.size())
            break;
        const uint32_t *h = g_g570Batch139GpuTris.data() + i * kG642TriStride;
        int64_t rowEdge[3] = {p.edgeBase[0], p.edgeBase[1], p.edgeBase[2]};
        for (int y = p.minY; y <= p.maxY; ++y)
        {
            ++g_g642Rows;
            const uint32_t packedSpan = g_g570Batch139Spans[
                static_cast<size_t>(p.spanOffset) + static_cast<size_t>(y - p.minY)];
            const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
            const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
            const uint32_t dy = static_cast<uint32_t>(y - p.minY);
            bool ok = xStart == p.minX && xEnd == p.maxX &&
                      h[kG649HMinX] == static_cast<uint32_t>(p.minX) &&
                      h[kG649HMaxX] == static_cast<uint32_t>(p.maxX);
            for (int k = 0; k < 3 && ok; ++k)
                ok = static_cast<uint32_t>(rowEdge[k]) ==
                     (h[kG649HEdge0 + static_cast<size_t>(k)] +
                      h[kG649HRowStep0 + static_cast<size_t>(k)] * dy);
            if (!ok && ++g_g642Bad <= 8ull)
                std::fprintf(stderr,
                             "[G642:vfy] BAD tri=%zu y=%d span=(%d,%d) bbox=(%d,%d) "
                             "e=(%08x,%08x,%08x) derived=(%08x,%08x,%08x)\n",
                             i, y, xStart, xEnd, p.minX, p.maxX,
                             static_cast<uint32_t>(rowEdge[0]),
                             static_cast<uint32_t>(rowEdge[1]),
                             static_cast<uint32_t>(rowEdge[2]),
                             h[kG649HEdge0] + h[kG649HRowStep0] * dy,
                             h[kG649HEdge1] + h[kG649HRowStep1] * dy,
                             h[kG649HEdge2] + h[kG649HRowStep2] * dy);
            rowEdge[0] += p.rowStep[0];
            rowEdge[1] += p.rowStep[1];
            rowEdge[2] += p.rowStep[2];
        }
    }
}

static bool g570Batch139BuildGpuPayloadPick()
{
    if (!g639PayParOn())
        return g570Batch139BuildGpuPayloadSerial();
    if (!g639PayParVerifyOn())
        return g570Batch139BuildGpuPayloadParallel();

    // Oracle: build in PARALLEL, snapshot, rebuild SERIALLY, compare byte for byte. The serial
    // arm writes last, so a defect in the parallel build cannot reach a pixel while this is armed.
    const bool rPar = g570Batch139BuildGpuPayloadParallel();
    std::vector<uint32_t> vTris = g_g570Batch139GpuTris;
    std::vector<uint32_t> vSpans = g_g570Batch139GpuSpans;
    std::vector<uint32_t> vOff = g_g570Batch139GpuTileOffsets;
    std::vector<uint32_t> vBins = g_g570Batch139GpuTileTris;
    const bool rSer = g570Batch139BuildGpuPayloadSerial();
    ++g_g639ParBatches;
    const bool eq = (rPar == rSer) && vTris == g_g570Batch139GpuTris &&
                    vSpans == g_g570Batch139GpuSpans &&
                    vOff == g_g570Batch139GpuTileOffsets &&
                    vBins == g_g570Batch139GpuTileTris;
    if (!eq)
    {
        ++g_g639ParBad;
        if (g_g639ParBad <= 8ull)
        {
            size_t firstTris = static_cast<size_t>(-1), firstSpans = static_cast<size_t>(-1),
                   firstBins = static_cast<size_t>(-1);
            for (size_t i = 0; i < vTris.size() && i < g_g570Batch139GpuTris.size(); ++i)
                if (vTris[i] != g_g570Batch139GpuTris[i]) { firstTris = i; break; }
            for (size_t i = 0; i < vSpans.size() && i < g_g570Batch139GpuSpans.size(); ++i)
                if (vSpans[i] != g_g570Batch139GpuSpans[i]) { firstSpans = i; break; }
            for (size_t i = 0; i < vBins.size() && i < g_g570Batch139GpuTileTris.size(); ++i)
                if (vBins[i] != g_g570Batch139GpuTileTris[i]) { firstBins = i; break; }
            std::fprintf(stderr,
                         "[G639:paypar] MISMATCH n=%llu tris=%zu/%zu@%zd spans=%zu/%zu@%zd "
                         "off=%zu/%zu bins=%zu/%zu@%zd rPar=%u rSer=%u\n",
                         (unsigned long long)g_g639ParBad,
                         vTris.size(), g_g570Batch139GpuTris.size(), (ptrdiff_t)firstTris,
                         vSpans.size(), g_g570Batch139GpuSpans.size(), (ptrdiff_t)firstSpans,
                         vOff.size(), g_g570Batch139GpuTileOffsets.size(),
                         vBins.size(), g_g570Batch139GpuTileTris.size(), (ptrdiff_t)firstBins,
                         rPar ? 1u : 0u, rSer ? 1u : 0u);
        }
    }
    if (g639PayParStatOn() && (g_g639ParBatches % 600ull) == 0ull)
        std::fprintf(stderr, "[G639:paypar] batches=%llu bad=%llu\n",
                     (unsigned long long)g_g639ParBatches, (unsigned long long)g_g639ParBad);
    return rSer;
}

static bool g570Batch139BuildGpuPayload()
{
    const bool ok = g570Batch139BuildGpuPayloadPick();
    if (ok && g642DeriveActive() && g642StatOn())
    {
        ++g_g642Batches;
        // The words the legacy chain would have uploaded, minus the six header words per triangle
        // that replace them.
        g_g642SavedWords += static_cast<uint64_t>(g_g570Batch139Spans.size()) * 4ull -
                            std::min<uint64_t>(
                                static_cast<uint64_t>(g_g570Batch139Spans.size()) * 4ull,
                                static_cast<uint64_t>(g_g570Batch139Tris.size()) * 6ull);
        if (g642VerifyOn())
            g642VerifyBatch();
        if ((g_g642Batches % 600ull) == 0ull)
            std::fprintf(stderr,
                         "[G642:span] batches=%llu rows=%llu bad=%llu savedKB=%.1f\n",
                         (unsigned long long)g_g642Batches, (unsigned long long)g_g642Rows,
                         (unsigned long long)g_g642Bad,
                         static_cast<double>(g_g642SavedWords) * 4.0 / 1024.0);
    }
    return ok;
}

static bool g570Batch139BuildGpuPayloadSerial()
{
    constexpr int tileShift = 4;
    constexpr int tileSize = 1 << tileShift;
    constexpr int tilesX = static_cast<int>(kG403DisplayZW) / tileSize;
    constexpr int tilesY = static_cast<int>(kG403DisplayZH) / tileSize;
    constexpr size_t triStride = kG642TriStride;
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

    const bool derive = g642DeriveActive();
    const auto fitsI32 = [](int64_t value) {
        return value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
    };
    for (size_t triIndex = 0; triIndex < g_g570Batch139Tris.size(); ++triIndex)
    {
        const G570Batch139Tri &p = g_g570Batch139Tris[triIndex];
        const uint32_t gpuSpanOffset =
            derive ? 0u : static_cast<uint32_t>(g_g570Batch139GpuSpans.size() / 4u);
        if (!fitsI32(p.step[0]) || !fitsI32(p.step[1]) || !fitsI32(p.step[2]))
            return false;

        // G649: hot-first, quad-aligned (see the layout note beside kG642TriStride). Appended in
        // SLOT order so this stays a pure sequential append, exactly as before.
        const uint32_t packedMode = static_cast<uint32_t>(p.topLeftMask) |
            (p.delta > 0 ? 0x100u : 0u);
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.minX));      // kG649HMinX
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.maxX));      // kG649HMaxX
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.minY));      // kG649HMinY
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.maxY));      // kG649HMaxY
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[0]));   // kG649HStep0
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[1]));   // kG649HStep1
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.step[2]));   // kG649HStep2
        g_g570Batch139GpuTris.push_back(packedMode);                         // kG649HMode
        // G642's six words — see the note beside g642DeriveSpanOn.
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.rowStep[0]));  // kG649HRowStep0
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.rowStep[1]));  // kG649HRowStep1
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.rowStep[2]));  // kG649HRowStep2
        g_g570Batch139GpuTris.push_back(gpuSpanOffset);                        // kG649HSpanOff
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.edgeBase[0])); // kG649HEdge0
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.edgeBase[1])); // kG649HEdge1
        g_g570Batch139GpuTris.push_back(static_cast<uint32_t>(p.edgeBase[2])); // kG649HEdge2
        g_g570Batch139GpuTris.push_back(0u);                                   // kG649HPad0
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.a0));          // kG649HA0
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.b0));          // kG649HB0
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.a1));          // kG649HA1
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.b1));          // kG649HB1
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.fx2));         // kG649HFx2
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.fy2));         // kG649HFy2
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.winding));     // kG649HWinding
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.invAbsDenom)); // kG649HInvDenom
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z0));          // kG649HZ0
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z1));          // kG649HZ1
        g_g570Batch139GpuTris.push_back(g570Batch139FloatBits(p.z2));          // kG649HZ2
        g_g570Batch139GpuTris.push_back(0u);                                   // kG649HPad1

        if (p.maxX < p.minX)
            continue;
        if (derive)
        {
            // G642: the O(1) four-corner guard, exactly equivalent to the union of the per-row
            // checks in the `else` arm below.
            if (p.maxY >= p.minY)
            {
                const int64_t dxMax =
                    std::max<int64_t>(0, static_cast<int64_t>(p.maxX) - p.minX);
                const int64_t dyMax = static_cast<int64_t>(p.maxY) - p.minY;
                for (int k = 0; k < 3; ++k)
                {
                    const int64_t eTop = p.edgeBase[k];
                    const int64_t eBot = p.edgeBase[k] + p.rowStep[k] * dyMax;
                    const int64_t adv = p.step[k] * dxMax;
                    if (!fitsI32(eTop) || !fitsI32(eBot) || !fitsI32(adv) ||
                        !fitsI32(eTop + adv) || !fitsI32(eBot + adv))
                        return false;
                }
            }
        }
        else
        {
        // G649 (P7): these ARE `p.edgeBase` and `p.rowStep` — same operands, same base.
        int64_t rowEdge[3] = {
            p.edgeBase[0], p.edgeBase[1], p.edgeBase[2],
        };
        const int64_t rowStep[3] = {
            p.rowStep[0], p.rowStep[1], p.rowStep[2],
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
        } // G642: end of the legacy per-row span build

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
    // G642: derive mode emits no span records. One word keeps image unit 3 bound with a valid
    // (never-sampled) 1024-word texture, so the backend's non-empty/1024-multiple contract holds.
    if (derive && g_g570Batch139GpuSpans.empty())
        g_g570Batch139GpuSpans.push_back(0u);
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

// ===== G638: static-batch memo for the 0x139 shadow payload (BRING-UP DEFAULT OFF) =====
//
// PREMISE (measured, `dragon` static tail, `[G638:prep]`): `g570Batch139Prepare` is **4.735 ms/f**
// of a 33.8 ms/f GS pole — 40% of the whole CPU-fallback dispatch window — and `[G638:prep] ^payload`
// puts 76% of that inside `g570Batch139BuildGpuPayload`. What it rebuilds is a PURE FUNCTION of
// `g_g144List`: the same ~4,544 shadow triangles are re-linearised into the same 20-word tri
// records, the same 4-word span records and the same 896-tile bin index every frame, and
// `[G147:gif]` reports byte-identical `tags` / `packedRegs` / `imageKB` window after window.
//
// ⭐ EXACT WITHOUT A HASH. The memo does not fingerprint the batch — it keeps the exact bytes of
// every field the builder reads and compares them with `memcmp`, so there is no collision to
// reason about. The correctness obligation is therefore ENUMERATION, not probability: a field the
// builder reads and this key omits is a false HIT and a wrong pixel. The read set below is taken
// from the builder line by line, and `DC2_G638_PAYCACHE_VERIFY=1` rebuilds anyway and compares all
// four output vectors byte for byte so a missed field cannot survive a verified run.
//
// ⛔ The gates ABOVE the key (env flags, `g203UniversalZEnabled`, `g404SharedZScope`, the null
// `g_g144LastGs`) are re-evaluated every frame and are NOT memoized — they read state the key
// cannot see. A memo hit is only taken when they all pass, exactly as a rebuild would require.
struct G638ShadowKey
{
    uint64_t test, zbuf, alpha, fba;
    float x0, y0, z0, x1, y1, z1, x2, y2, z2;
    uint32_t fbp, fbw, fbmsk, bits;
    uint16_t sx0, sx1, sy0, sy1, ofx, ofy;
    uint8_t r2, g2, b2, a2;
};

static std::vector<G638ShadowKey> g_g638ShadowKey;
static bool g_g638PayCacheValid = false;
static bool g_g638PayCacheResult = false;
static int g_g638PayCacheReject = 0;
static uint64_t g_g638PayCacheHits = 0ull, g_g638PayCacheMiss = 0ull, g_g638PayCacheBad = 0ull;

// ⛔ THE MEMO NEEDS ITS OWN STORAGE. `g570Batch139PrepareBuild` clears the six shipped output
// vectors on EVERY call, and `dragon`'s static tail runs ~12 CPU-fallback batches per frame of
// which only one is the 0x139 shadow batch — so the eleven others would wipe a memo that aliased
// them. These hold the last successful 0x139 payload and are copied back on a hit.
static std::vector<G570Batch139Tri> g_g638MemoTris;
static std::vector<uint32_t> g_g638MemoSpans, g_g638MemoGpuTris, g_g638MemoGpuSpans,
    g_g638MemoTileOffsets, g_g638MemoTileTris;

static void g638MemoStore()
{
    g_g638MemoTris = g_g570Batch139Tris;
    g_g638MemoSpans = g_g570Batch139Spans;
    g_g638MemoGpuTris = g_g570Batch139GpuTris;
    g_g638MemoGpuSpans = g_g570Batch139GpuSpans;
    g_g638MemoTileOffsets = g_g570Batch139GpuTileOffsets;
    g_g638MemoTileTris = g_g570Batch139GpuTileTris;
}

static void g638MemoRestore()
{
    g_g570Batch139Tris = g_g638MemoTris;
    g_g570Batch139Spans = g_g638MemoSpans;
    g_g570Batch139GpuTris = g_g638MemoGpuTris;
    g_g570Batch139GpuSpans = g_g638MemoGpuSpans;
    g_g570Batch139GpuTileOffsets = g_g638MemoTileOffsets;
    g_g570Batch139GpuTileTris = g_g638MemoTileTris;
}

static bool g638PayCacheOn()
{
    static const bool on = !envFlagEnabled("DC2_G638_NO_PAYCACHE") &&
                           (envFlagEnabled("DC2_G638_PAYCACHE") ||
                            envFlagEnabled("DC2_G638_PAYCACHE_VERIFY"));
    return on;
}

static bool g638PayCacheVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G638_PAYCACHE_VERIFY");
    return on;
}

static bool g638PayCacheStatOn()
{
    static const bool on = envFlagEnabled("DC2_G638_PAYCACHE_STAT") || g638PayCacheVerifyOn();
    return on;
}

// Field-group difference census. Namespace scope, never a function-local `static const bool` —
// this is read once per ENTRY (thousands per frame) and MSVC emits a per-access TLS
// `_Init_global_epoch` guard for the function-local form.
static const bool g_g638KeyDiffOn = (std::getenv("DC2_G638_PAYCACHE_STAT") != nullptr) ||
                                    (std::getenv("DC2_G638_PAYCACHE_VERIFY") != nullptr);
// [0]=xy [1]=z [2]=rgba [3]=state [4]=scissor/offset [5]=entries differing [6]=entries compared
static uint64_t g_g638KeyDiff[7] = {0, 0, 0, 0, 0, 0, 0};

// Build this frame's key over `g_g144List` and report whether it equals the stored one. Always
// leaves the stored key equal to this frame's, so a miss re-arms the memo for the next frame.
static bool g638ShadowKeySameAndStore(const std::vector<G144Entry> &entries)
{
    const size_t n = entries.size();
    bool same = (g_g638ShadowKey.size() == n);
    if (!same)
        g_g638ShadowKey.assign(n, G638ShadowKey{});
    for (size_t i = 0; i < n; ++i)
    {
        const G144Entry &e = entries[i];
        G638ShadowKey k{}; // zero-initialised, so trailing padding is identical in both keys
        k.test = e.ctx.test;
        k.zbuf = e.ctx.zbuf;
        k.alpha = e.ctx.alpha;
        k.fba = e.ctx.fba;
        k.x0 = e.v[0].x; k.y0 = e.v[0].y; k.z0 = e.v[0].z;
        k.x1 = e.v[1].x; k.y1 = e.v[1].y; k.z1 = e.v[1].z;
        k.x2 = e.v[2].x; k.y2 = e.v[2].y; k.z2 = e.v[2].z;
        k.fbp = e.ctx.frame.fbp;
        k.fbw = e.ctx.frame.fbw;
        k.fbmsk = e.ctx.frame.fbmsk;
        k.bits = static_cast<uint32_t>(e.ctx.frame.psm) |
                 (static_cast<uint32_t>(e.prim.type) << 8u) |
                 (e.prim.iip ? 1u << 16u : 0u) |
                 (e.prim.tme ? 1u << 17u : 0u) |
                 (e.prim.abe ? 1u << 18u : 0u) |
                 (e.prim.fge ? 1u << 19u : 0u) |
                 (e.pabe ? 1u << 20u : 0u);
        k.sx0 = e.ctx.scissor.x0; k.sx1 = e.ctx.scissor.x1;
        k.sy0 = e.ctx.scissor.y0; k.sy1 = e.ctx.scissor.y1;
        k.ofx = e.ctx.xyoffset.ofx; k.ofy = e.ctx.xyoffset.ofy;
        k.r2 = e.v[2].r; k.g2 = e.v[2].g; k.b2 = e.v[2].b; k.a2 = e.v[2].a;
        if (std::memcmp(&g_g638ShadowKey[i], &k, sizeof(k)) != 0)
        {
            // G638: a NULL memo is only useful if it says WHAT moved. Bucket the difference by
            // field group, because the groups feed different halves of the payload: `xy` and
            // `scissor` decide the spans AND the 896-tile bin index, `z` and `rgba` only reach the
            // 20-word tri header, and `state` decides admission. A batch whose xy is stable but
            // whose z moves would still admit a PARTIAL memo of the spans and the bin index.
            if (g_g638KeyDiffOn)
            {
                const G638ShadowKey &o = g_g638ShadowKey[i];
                if (o.x0 != k.x0 || o.y0 != k.y0 || o.x1 != k.x1 ||
                    o.y1 != k.y1 || o.x2 != k.x2 || o.y2 != k.y2)
                    ++g_g638KeyDiff[0];
                if (o.z0 != k.z0 || o.z1 != k.z1 || o.z2 != k.z2)
                    ++g_g638KeyDiff[1];
                if (o.r2 != k.r2 || o.g2 != k.g2 || o.b2 != k.b2 || o.a2 != k.a2)
                    ++g_g638KeyDiff[2];
                if (o.test != k.test || o.zbuf != k.zbuf || o.alpha != k.alpha ||
                    o.fba != k.fba || o.fbp != k.fbp || o.fbw != k.fbw ||
                    o.fbmsk != k.fbmsk || o.bits != k.bits)
                    ++g_g638KeyDiff[3];
                if (o.sx0 != k.sx0 || o.sx1 != k.sx1 || o.sy0 != k.sy0 ||
                    o.sy1 != k.sy1 || o.ofx != k.ofx || o.ofy != k.ofy)
                    ++g_g638KeyDiff[4];
                ++g_g638KeyDiff[5];
            }
            same = false;
        }
        if (g_g638KeyDiffOn)
            ++g_g638KeyDiff[6];
        g_g638ShadowKey[i] = k;
    }
    return same;
}

static void g638PayCacheReport()
{
    const uint64_t tot = g_g638PayCacheHits + g_g638PayCacheMiss;
    std::fprintf(stderr,
                 "[G638:paycache] batches=%llu hits=%llu misses=%llu hitRate=%.2f%% bad=%llu\n",
                 (unsigned long long)tot, (unsigned long long)g_g638PayCacheHits,
                 (unsigned long long)g_g638PayCacheMiss,
                 tot ? 100.0 * (double)g_g638PayCacheHits / (double)tot : 0.0,
                 (unsigned long long)g_g638PayCacheBad);
    if (g_g638KeyDiffOn && g_g638KeyDiff[6] != 0ull)
    {
        static const char *const kName[5] = {"xy", "z", "rgba", "state", "scissor"};
        const double denom = static_cast<double>(g_g638KeyDiff[6]);
        std::fprintf(stderr,
                     "[G638:keydiff] entries=%llu differing=%llu (%.2f%%)",
                     (unsigned long long)g_g638KeyDiff[6],
                     (unsigned long long)g_g638KeyDiff[5],
                     100.0 * (double)g_g638KeyDiff[5] / denom);
        for (int i = 0; i < 5; ++i)
            std::fprintf(stderr, " %s=%.2f%%", kName[i],
                         100.0 * (double)g_g638KeyDiff[i] / denom);
        std::fprintf(stderr, "\n");
    }
}

static bool g570Batch139PrepareBuild();

static bool g570Batch139Prepare()
{
    if (!g638PayCacheOn())
        return g570Batch139PrepareBuild();

    // Re-evaluate the un-memoizable gates, INCLUDING the front-entry shape test that identifies a
    // 0x139 shadow batch at all. Every other batch bypasses the memo entirely and goes to the
    // builder unchanged — which is why the memo keeps its own copy of the outputs (see above).
    // This is `g570Batch139PrepareBuild`'s own reason-1 and reason-2 test, unchanged.
    const G144Entry *front = g_g144List.empty() ? nullptr : &g_g144List.front();
    const bool shadowBatch =
        g570Batch139On() && g570Cpu139Quiet() && !g570Cpu139On() &&
        !envFlagEnabled("DC2_G408_LEGACY_SHADOW_COVERAGE") &&
        g203UniversalZEnabled() && g_g144LastGs != nullptr && g_g144List.size() >= 2u &&
        front != nullptr && front->ctx.frame.fbp == 0x139u && front->ctx.frame.fbw == 8u &&
        front->ctx.frame.psm == GS_PSM_CT32 && front->prim.type == GS_PRIM_SPRITE &&
        !front->prim.tme && !front->prim.abe && !front->prim.fge &&
        front->ctx.test == 0x3000aull && front->ctx.zbuf == 0x1010000d0ull &&
        g404SharedZScope(0x139u, 8u, 0x0d0u, 1u);
    if (!shadowBatch)
        return g570Batch139PrepareBuild();

    const bool same = g638ShadowKeySameAndStore(g_g144List);
    const bool hit = same && g_g638PayCacheValid;
    if (hit && !g638PayCacheVerifyOn())
    {
        ++g_g638PayCacheHits;
        if (g638PayCacheStatOn() && ((g_g638PayCacheHits + g_g638PayCacheMiss) % 600ull) == 0ull)
            g638PayCacheReport();
        g638MemoRestore();
        g_g570Batch139RejectReason = g_g638PayCacheReject;
        return g_g638PayCacheResult;
    }

    // VERIFY arm: on what WOULD have been a hit, snapshot the memoized outputs, rebuild, and
    // compare all four vectors byte for byte. A single mismatch means the key is missing a field.
    std::vector<G570Batch139Tri> vTris;
    std::vector<uint32_t> vSpans, vGpuTris, vGpuSpans, vTileOff, vTileTris;
    const bool checking = hit && g638PayCacheVerifyOn();
    int prevReject = g_g638PayCacheReject;
    bool prevResult = g_g638PayCacheResult;
    if (checking)
    {
        // Compare against the MEMO, not the live vectors: on a hit the live ones still hold
        // whichever non-shadow batch ran last.
        vTris = g_g638MemoTris;
        vSpans = g_g638MemoSpans;
        vGpuTris = g_g638MemoGpuTris;
        vGpuSpans = g_g638MemoGpuSpans;
        vTileOff = g_g638MemoTileOffsets;
        vTileTris = g_g638MemoTileTris;
    }

    const bool r = g570Batch139PrepareBuild();
    g638MemoStore();

    if (checking)
    {
        const bool eq =
            (r == prevResult) && (g_g570Batch139RejectReason == prevReject) &&
            vSpans == g_g570Batch139Spans && vGpuTris == g_g570Batch139GpuTris &&
            vGpuSpans == g_g570Batch139GpuSpans &&
            vTileOff == g_g570Batch139GpuTileOffsets &&
            vTileTris == g_g570Batch139GpuTileTris &&
            vTris.size() == g_g570Batch139Tris.size() &&
            (vTris.empty() || std::memcmp(vTris.data(), g_g570Batch139Tris.data(),
                                          vTris.size() * sizeof(G570Batch139Tri)) == 0);
        if (!eq)
        {
            ++g_g638PayCacheBad;
            if (g_g638PayCacheBad <= 8ull)
                std::fprintf(stderr,
                             "[G638:paycache] MISMATCH n=%llu entries=%zu tris=%zu/%zu "
                             "spans=%zu/%zu gpuTris=%zu/%zu gpuSpans=%zu/%zu tileTris=%zu/%zu "
                             "result=%u/%u reject=%d/%d\n",
                             (unsigned long long)g_g638PayCacheBad, g_g144List.size(),
                             vTris.size(), g_g570Batch139Tris.size(),
                             vSpans.size(), g_g570Batch139Spans.size(),
                             vGpuTris.size(), g_g570Batch139GpuTris.size(),
                             vGpuSpans.size(), g_g570Batch139GpuSpans.size(),
                             vTileTris.size(), g_g570Batch139GpuTileTris.size(),
                             prevResult ? 1u : 0u, r ? 1u : 0u,
                             prevReject, g_g570Batch139RejectReason);
        }
        ++g_g638PayCacheHits;
    }
    else
    {
        ++g_g638PayCacheMiss;
    }

    g_g638PayCacheValid = true;
    g_g638PayCacheResult = r;
    g_g638PayCacheReject = g_g570Batch139RejectReason;
    if (g638PayCacheStatOn() && ((g_g638PayCacheHits + g_g638PayCacheMiss) % 600ull) == 0ull)
        g638PayCacheReport();
    return r;
}

// ===== ⭐ G649 (ROADMAP P2): the per-triangle setup loop, in parallel ==========================
//
// PREMISE — ⚠️ RE-TAKEN, because the ROADMAP's number was PRE-G648. `[G638:prep]` on `dragon`'s
// static tail, measured on the shipped G648 binary:
//     g570gpubatch  44.42%  201.5 us/batch
//     poolreplay    35.46%  160.9 us/batch
//     g570prep      16.60%   75.3 us/batch
//       ^tris        8.13%   36.9 us/batch   <-- ROADMAP quotes 19.83% / 103.5 us/batch
//       ^payload     6.95%   31.5 us/batch
//     clut           3.46%   15.7 us/batch
// G648 already deleted the span-array push that made up two thirds of `^tris`, so this lever's
// ceiling is a THIRD of what ROADMAP P2 priced. What remains is still the largest SERIAL item in
// `g570prep` and it still sits on the GS pole thread, which P1 re-qualified this phase at an
// injection sensitivity of **0.953** — so it still converts.
//
// ⭐ WHY IT PARALLELISES EXACTLY (G639's argument, one step simpler). After G648 the loop body
// writes exactly ONE output slot per input entry — `g_g570Batch139Tris[i]` for `g_g144List[i+1]` —
// and reads nothing any other iteration writes. There is no prefix pass at all, because
// `p.spanOffset` is the running `g_g570Batch139Spans.size()`, which is CONSTANT ZERO in the deferred
// configuration (G648 never pushes a span). So the fences can be anywhere: contiguous, disjoint,
// covering [0,n) once. That is why this arm is admitted ONLY when `deferSpans` holds — in the
// eager configuration `spanOffset` IS a serial dependency and the original loop runs.
//
// ⚠️ THE FOUR REJECT REASONS. The serial loop `return false`s on the first refusing triangle, so the
// reported `g_g570Batch139RejectReason` is the LOWEST-index failure. The parallel arm records
// `(index << 3) | reason` into one atomic under a min-CAS and applies it after the join, which
// reproduces that exactly — including reason 4 winning over reason 5 inside one triangle, because
// the helper below tests them in the original order.
//
// Rollback: DC2_G649_NO_TRISPAR=1 -> the original serial loop, verbatim.
static bool g649TrisParOn()
{
    static const bool on = !envFlagEnabled("DC2_G649_NO_TRISPAR");
    return on;
}
static bool g649TrisParVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G649_TRISPAR_VERIFY");
    return on;
}
static bool g649TrisParStatOn()
{
    static const bool on = envFlagEnabled("DC2_G649_TRISPAR_STAT") || g649TrisParVerifyOn();
    return on;
}

// Field-wise, because `G570Batch139Tri` carries padding bytes: the parallel arm's elements come
// from `vector::resize` (value-init, padding zeroed) and the oracle's from a default-init local
// (padding indeterminate), so a `memcmp` of the whole struct could differ on bytes nothing reads.
static bool g649TriEq(const G570Batch139Tri &a, const G570Batch139Tri &b)
{
    return a.minX == b.minX && a.maxX == b.maxX && a.minY == b.minY && a.maxY == b.maxY &&
           std::memcmp(a.step, b.step, sizeof(a.step)) == 0 &&
           std::memcmp(a.rowStep, b.rowStep, sizeof(a.rowStep)) == 0 &&
           std::memcmp(a.edgeBase, b.edgeBase, sizeof(a.edgeBase)) == 0 &&
           a.topLeftMask == b.topLeftMask && a.delta == b.delta &&
           a.spanOffset == b.spanOffset &&
           std::memcmp(&a.a0, &b.a0, sizeof(float)) == 0 &&
           std::memcmp(&a.b0, &b.b0, sizeof(float)) == 0 &&
           std::memcmp(&a.a1, &b.a1, sizeof(float)) == 0 &&
           std::memcmp(&a.b1, &b.b1, sizeof(float)) == 0 &&
           std::memcmp(&a.fx2, &b.fx2, sizeof(float)) == 0 &&
           std::memcmp(&a.fy2, &b.fy2, sizeof(float)) == 0 &&
           std::memcmp(&a.winding, &b.winding, sizeof(float)) == 0 &&
           std::memcmp(&a.invAbsDenom, &b.invAbsDenom, sizeof(float)) == 0 &&
           std::memcmp(&a.z0, &b.z0, sizeof(float)) == 0 &&
           std::memcmp(&a.z1, &b.z1, sizeof(float)) == 0 &&
           std::memcmp(&a.z2, &b.z2, sizeof(float)) == 0;
}

static std::vector<int> g_g649TrisBounds;
static std::atomic<uint64_t> g_g649TrisFail{~0ull};
static uint64_t g_g649TrisBatches = 0ull, g_g649TrisCount = 0ull, g_g649TrisBad = 0ull;

static inline bool g649FrameIs139(const G144Entry &e)
{
    return e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
           e.ctx.frame.psm == GS_PSM_CT32;
}

// The per-triangle body, shared VERBATIM by the serial and the parallel arm so the arithmetic
// exists in exactly one place. Returns 0 on success (`degenerate` reports the |denom| test, which
// the caller needs because only the non-degenerate branch owns span rows), or the reject reason.
static inline int g649SetupTri(const G144Entry &e, uint32_t spanOffsetIn,
                               G570Batch139Tri &p, bool &degenerate)
{
    degenerate = false;
    const uint8_t alphaMode = static_cast<uint8_t>(e.ctx.alpha & 0xffu);
    const GSVertex &v0 = e.v[0], &v1 = e.v[1], &v2 = e.v[2];
    const bool flatUnit = !e.prim.iip && v2.r == 1u && v2.g == 1u &&
                          v2.b == 1u && v2.a == 0x40u;
    if (!g649FrameIs139(e) || e.prim.type != GS_PRIM_TRIFAN || e.prim.tme ||
        !e.prim.abe || e.prim.fge || e.ctx.test != 0x5000aull ||
        e.ctx.zbuf != 0x1010000d0ull ||
        (alphaMode != 0x62u && alphaMode != 0x68u) ||
        static_cast<uint8_t>((e.ctx.alpha >> 32u) & 0xffu) != 0x80u ||
        !flatUnit || e.ctx.frame.fbmsk != 0u || e.ctx.fba != 0u || e.pabe)
        return 4;

    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    const float x0 = v0.x - static_cast<float>(ofx);
    const float y0 = v0.y - static_cast<float>(ofy);
    const float x1 = v1.x - static_cast<float>(ofx);
    const float y1 = v1.y - static_cast<float>(ofy);
    const float x2 = v2.x - static_cast<float>(ofx);
    const float y2 = v2.y - static_cast<float>(ofy);
    const float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);

    // G649 (P7): `ofx/ofy/rawX/rawY` are no longer stored — the CPU band replay re-reads them from
    // this very entry through `g649TriRawAt`, which is bit-exact because they are literal copies.
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
        return 5;

    // G649: `|=` below needs a zero base, and the parallel arm's storage is recycled.
    p.topLeftMask = 0u;
    p.delta = (alphaMode == 0x68u) ? 1 : -1;
    if (std::fabs(denom) >= 0.001f)
    {
        // G649 (P7): `fx/fy/sign` are LOCALS now. They exist only to derive `step`, `rowStep`,
        // `edgeBase` and `topLeftMask` below; every later consumer reads those three arrays through
        // `g649EdgesAt`, which is the same value by the affine identity.
        const int64_t fx[3] = {
            static_cast<int64_t>(std::llround(x0 * 16.0f)),
            static_cast<int64_t>(std::llround(x1 * 16.0f)),
            static_cast<int64_t>(std::llround(x2 * 16.0f)),
        };
        const int64_t fy[3] = {
            static_cast<int64_t>(std::llround(y0 * 16.0f)),
            static_cast<int64_t>(std::llround(y1 * 16.0f)),
            static_cast<int64_t>(std::llround(y2 * 16.0f)),
        };
        const int sign = denom >= 0.0f ? 1 : -1;
        const auto topLeft = [&](int a, int b) {
            int64_t dx = fx[b] - fx[a];
            int64_t dy = fy[b] - fy[a];
            if (sign < 0) { dx = -dx; dy = -dy; }
            return dy < 0 || (dy == 0 && dx > 0);
        };
        if (topLeft(1, 2)) p.topLeftMask |= 1u;
        if (topLeft(2, 0)) p.topLeftMask |= 2u;
        if (topLeft(0, 1)) p.topLeftMask |= 4u;
        p.step[0] = -(fy[2] - fy[1]) * 16 * sign;
        p.step[1] = -(fy[0] - fy[2]) * 16 * sign;
        p.step[2] = -(fy[1] - fy[0]) * 16 * sign;
        p.rowStep[0] = (fx[2] - fx[1]) * 16 * sign;
        p.rowStep[1] = (fx[0] - fx[2]) * 16 * sign;
        p.rowStep[2] = (fx[1] - fx[0]) * 16 * sign;
        p.a0 = y1 - y2; p.b0 = x2 - x1;
        p.a1 = y2 - y0; p.b1 = x0 - x2;
        p.fx2 = x2; p.fy2 = y2;
        p.winding = denom < 0.0f ? -1.0f : 1.0f;
        p.invAbsDenom = 1.0f / std::fabs(denom);
        p.z0 = v0.z; p.z1 = v1.z; p.z2 = v2.z;
        p.spanOffset = spanOffsetIn;
        const int64_t baseSx = static_cast<int64_t>(p.minX) * 16;
        const int64_t baseSy = static_cast<int64_t>(p.minY) * 16;
        p.edgeBase[0] =
            ((fx[2] - fx[1]) * (baseSy - fy[1]) -
             (fy[2] - fy[1]) * (baseSx - fx[1])) * sign;
        p.edgeBase[1] =
            ((fx[0] - fx[2]) * (baseSy - fy[2]) -
             (fy[0] - fy[2]) * (baseSx - fx[2])) * sign;
        p.edgeBase[2] =
            ((fx[1] - fx[0]) * (baseSy - fy[0]) -
             (fy[1] - fy[0]) * (baseSx - fx[0])) * sign;
    }
    else
    {
        degenerate = true;
        // G649: the serial arm reached here with a freshly DEFAULT-CONSTRUCTED `p` and set only the
        // bbox, `delta` and this marker; every other member kept its default. The parallel arm's
        // storage is recycled, so restore exactly that state rather than inherit the last batch's.
        const int keepMinX = p.minX, keepMinY = p.minY, keepMaxY = p.maxY;
        const int8_t keepDelta = p.delta;
        p = G570Batch139Tri{};
        p.minX = keepMinX;
        p.minY = keepMinY;
        p.maxY = keepMaxY;
        p.delta = keepDelta;
        p.maxX = -1; // generic drawTriangle drops the same degenerate primitive
    }
    return 0;
}

static bool g570Batch139PrepareBuild()
{
    g_g570Batch139RejectReason = 1;
    // ⭐ G649 (P2): `g_g570Batch139Tris` is NOT cleared here any more.
    //
    // `[G649:trispar]` measures **4,588–4,763 triangles per batch**, so this array is ~1.09 MB, and
    // `std::vector::resize` VALUE-INITIALISES: clearing it here and resizing it in the parallel arm
    // would write that megabyte of zeros SERIALLY on the pole thread before the parallel job wrote
    // it again with data — the same order as the whole `^tris` lap, i.e. it would eat the lever.
    // Leaving the previous batch's SIZE in place makes `resize(n)` grow-only, and `n` barely moves
    // between batches on this route, so a steady-state batch pays nothing.
    //
    // ⚠️ Two consequences, both handled:
    //   * every path that returns false below still clears it explicitly, and no consumer reads the
    //     array without `g570PreparedBatch` (rasterizer_tilebin_capture.inc) being true, so an
    //     early return leaves storage that nothing looks at;
    //   * element storage is RECYCLED, so `g649SetupTri` may not assume a default-constructed `p`.
    //     It resets `topLeftMask` and rebuilds the whole default state on the degenerate path.
    // ⚠️⚠️ A FIRST ATTEMPT parked the buffer in a persistent scratch and swapped. It does not work:
    // ~15 of every 16 dispatch windows reject at reason 2 BEFORE the loop, and each of those swapped
    // and cleared, so after two consecutive rejects BOTH buffers were empty and the next successful
    // batch paid the full value-init anyway.
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
    // G648: defer ONLY when the GPU path both serves this batch and derives its own spans, so the
    // uploaded table is not the thing being skipped. The G642 oracle needs the eager array to
    // compare against, so it disables the deferral outright.
    const bool deferSpans =
        gpuSpans && g642DeriveActive() && g648LazySpanOn() && !g642VerifyOn();
    g_g648SpansDeferred = deferSpans;
    // G638b sub-lap (no-op unless DC2_G638_PREP=1): `[G638:prep] g570prep` is 4.735 ms/f of
    // `dragon`'s static tail — 40% of the whole CPU-fallback dispatch window and the largest
    // single-threaded item on the GS pole. Split it into the per-triangle setup loop below and
    // g570Batch139BuildGpuPayload, because they need different levers.
    const auto g638TrisT0 = g_g638PrepCensusOn ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    const size_t g649N = g_g144List.size() - 1u;
    // ⭐ G649 (P2): the parallel arm. Admitted only in the DEFERRED configuration — see the note
    // above `g649SetupTri` for why `spanOffset` is the whole exactness question — and only when the
    // pool actually has lanes and the batch is big enough to pay for one dispatch.
    const bool g649Par = deferSpans && g649TrisParOn() && g_g144Threads > 1 && g649N >= 64u;
    if (g649Par)
    {
        // Grow-only IN PLACE: `resize` value-initialises only the GROWTH (the array was not cleared
        // at the top), and shrinking is O(1) for a trivially destructible element. In the steady
        // state `n` barely moves, so a batch pays nothing here instead of writing 1.09 MB of zeros.
        g_g570Batch139Tris.resize(g649N);
        int lanes = g_g144Threads;
        const int cap = GSRowPool::instance().laneCap();
        if (lanes > cap)
            lanes = cap;
        // Over-decompose 4x the lane count and let every participant, the caller included, steal —
        // G637 §5's law: a triangle's cost is not uniform, and a work estimator is both redundant
        // and harmful beside a dynamic claimer.
        int chunks = lanes * 4;
        if (chunks > static_cast<int>(g649N))
            chunks = static_cast<int>(g649N);
        if (chunks < 1)
            chunks = 1;
        g_g649TrisBounds.resize(static_cast<size_t>(chunks) + 1u);
        for (int c = 0; c <= chunks; ++c)
            g_g649TrisBounds[static_cast<size_t>(c)] =
                static_cast<int>(static_cast<int64_t>(g649N) * c / chunks);
        g_g649TrisFail.store(~0ull, std::memory_order_relaxed);
        const std::function<void(int, int)> g649Build = [&](int a, int b) {
            for (int i = a; i <= b; ++i)
            {
                bool degenerate = false;
                const int reason =
                    g649SetupTri(g_g144List[static_cast<size_t>(i) + 1u], 0u,
                                 g_g570Batch139Tris[static_cast<size_t>(i)], degenerate);
                if (reason != 0)
                {
                    // Lowest index wins, so the reported reason is the serial loop's.
                    const uint64_t packed =
                        (static_cast<uint64_t>(static_cast<uint32_t>(i)) << 3) |
                        static_cast<uint64_t>(static_cast<uint32_t>(reason));
                    uint64_t cur = g_g649TrisFail.load(std::memory_order_relaxed);
                    while (packed < cur &&
                           !g_g649TrisFail.compare_exchange_weak(cur, packed,
                                                                 std::memory_order_relaxed))
                    {
                    }
                    return;
                }
            }
        };
        GSRowPool::instance().runCoop(g_g649TrisBounds.data(), chunks, lanes - 1, g649Build);
        const uint64_t g649Fail = g_g649TrisFail.load(std::memory_order_relaxed);
        if (g649Fail != ~0ull)
        {
            g_g570Batch139RejectReason = static_cast<int>(g649Fail & 7ull);
            g_g570Batch139Tris.clear();
            g_g570Batch139Spans.clear();
            return false;
        }
        // Oracle: rebuild the SAME list serially, through the SAME helper, and compare field by
        // field. A divergence can only come from the partition, since the arithmetic is shared.
        if (g649TrisParVerifyOn())
        {
            std::vector<G570Batch139Tri> ref;
            ref.reserve(g649N);
            bool refOk = true;
            for (size_t i = 1; i < g_g144List.size() && refOk; ++i)
            {
                G570Batch139Tri rp;
                bool degenerate = false;
                if (g649SetupTri(g_g144List[i], 0u, rp, degenerate) != 0)
                    refOk = false;
                else
                    ref.push_back(rp);
            }
            bool eq = refOk && ref.size() == g_g570Batch139Tris.size();
            size_t firstBad = ref.size();
            for (size_t i = 0; eq && i < ref.size(); ++i)
                if (!g649TriEq(ref[i], g_g570Batch139Tris[i]))
                {
                    eq = false;
                    firstBad = i;
                }
            if (!eq && ++g_g649TrisBad <= 8ull)
                std::fprintf(stderr,
                             "[G649:trispar] MISMATCH refOk=%d n=%zu par=%zu firstBad=%zu\n",
                             refOk ? 1 : 0, ref.size(), g_g570Batch139Tris.size(), firstBad);
        }
        if (g649TrisParStatOn())
        {
            ++g_g649TrisBatches;
            g_g649TrisCount += g649N;
            if ((g_g649TrisBatches % 600ull) == 0ull)
                std::fprintf(stderr,
                             "[G649:trispar] batches=%llu tris/batch=%.1f chunks=%d lanes=%d "
                             "bad=%llu\n",
                             static_cast<unsigned long long>(g_g649TrisBatches),
                             static_cast<double>(g_g649TrisCount) /
                                 static_cast<double>(g_g649TrisBatches),
                             chunks, lanes,
                             static_cast<unsigned long long>(g_g649TrisBad));
        }
    }
    else
    {
    // G649: the top no longer clears, so the serial arm clears its own output before appending.
    g_g570Batch139Tris.clear();
    g_g570Batch139Tris.reserve(g_g144List.size() - 1u);
    for (size_t i = 1; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        G570Batch139Tri p;
        bool degenerate = false;
        const int reason = g649SetupTri(
            e, static_cast<uint32_t>(g_g570Batch139Spans.size()), p, degenerate);
        if (reason != 0)
        {
            g_g570Batch139RejectReason = reason;
            g_g570Batch139Tris.clear();
            g_g570Batch139Spans.clear();
            return false;
        }
        if (!degenerate)
        {
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
            // G648: in the deferred configuration every iteration of this loop would push the SAME
            // constant `(minX, maxX)` that `p` already carries, and `rowEdge` / `spanBounds` are
            // only read by the tight-span arm. Skip the whole thing; `g648SpanAt` reads `p`.
            if (!deferSpans)
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
        g_g570Batch139Tris.push_back(p);
    }
    }
    if (g_g638PrepCensusOn)
        g638PrepNote(kG638PhG570Tris, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g638TrisT0).count()));
    if (g_g570Batch139Tris.empty())
    {
        g_g570Batch139RejectReason = 6;
        return false;
    }
    const auto g638PayloadT0 = g_g638PrepCensusOn ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
    const bool g638PayloadOk = !g570Batch139GpuOn() || g570Batch139BuildGpuPayload();
    if (g_g638PrepCensusOn)
        g638PrepNote(kG638PhG570Payload, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g638PayloadT0).count()));
    if (!g638PayloadOk)
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
        // G649 (P7): the unscaled vertices come straight from the entry now — the setup loop no
        // longer copies them into the triangle. Same subtraction, same operands, same order.
        G649TriRaw raw;
        g649TriRawAt(triIndex, raw);
        const float lx0 = raw.lx[0];
        const float ly0 = raw.ly[0];
        const float lx1 = raw.lx[1];
        const float ly1 = raw.ly[1];
        const float lx2 = raw.lx[2];
        const float ly2 = raw.ly[2];
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
            const uint32_t packedSpan = g648SpanAt(p, y); // G648
            const int xStart = static_cast<int16_t>(packedSpan & 0xffffu);
            const int xEnd = static_cast<int16_t>(packedSpan >> 16u);
            // G649 (P7): the same three integers, from the header's affine form.
            int64_t edgeStart[3];
            g649EdgesAt(p, xStart, y, edgeStart);
            int64_t edge0 = edgeStart[0];
            int64_t edge1 = edgeStart[1];
            int64_t edge2 = edgeStart[2];
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
    // G641: `rect*` is the producer's own tile bbox when the window lever is armed, and the whole
    // surface otherwise. `g641ShadowRect` returns false for anything it cannot describe, so every
    // refusal falls straight back onto the legacy whole-surface submit.
    int rx = 0, ry = 0, rw = static_cast<int>(kG403DisplayZW), rh = static_cast<int>(kG403DisplayZH);
    g641ShadowRect(rx, ry, rw, rh);
    const bool ok = g570_backend_shadow139(
        color, g403DisplayZBuf(), g_g570Batch139GpuTris,
        g_g570Batch139GpuSpans, g_g570Batch139GpuTileOffsets,
        g_g570Batch139GpuTileTris, out, rx, ry, rw, rh,
        // G642: the producer is authoritative about which span mode its payload encodes; the
        // backend must never re-derive the predicate from env flags of its own.
        g642DeriveActive() ? 1 : 0);
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
