// ===== G492: ceiling probes for the two unpriced blocking edges =================================
//
// PREMISE (G491 §3, re-taken on the current binary before a line was written — the G476 law).
//
//   [G447:edge] windowMs/f=23.0-23.7 | backend-render=3.48-3.70/34.0
//               backend-readcolor=3.19-3.39/6.0 backend-writecolor=0.37-0.42/11.0
//               backend-copy=0.10-0.11/2.0 backend-other=2.83-3.12/20.0
//               | blocked=10.67-11.06 cpu=12.34-12.75 (46-47% blocked)
//   DC2_G419_AB=rb_all -> delta=-2.748 ms/f (t=-10.15, 6/6) for a 3.3 ms/f caller-side wait.
//
// The GS worker's span IS the frame (sensitivity 0.95) and 46% of it is ~73 SERIALIZED GL round
// trips. Exactly one of those five buckets has ever been priced: `rb_all` converted 83% of the
// readback's wait into frame time. `backend-render` (3.5 ms/f over 34 items) and `backend-other`
// (2.8 ms/f over 20) are the same shape and have never had a within-process ceiling of their own,
// so the arc has been designing an overlap executor against an unmeasured bucket.
//
// A CEILING COSTS ONE RUN AND A DESIGN COSTS A PHASE. These two probes are built on the exact
// `rb_all` pattern (g454SkipReadbackAt): keep every byte of the front end's batch building, its
// argument validation, its window publication and its downstream unpack/publication, and delete
// only the queue push and the blocking wait at ONE named edge. The candidate output is knowingly
// stale — nothing is rendered / no view texture is refreshed on a candidate frame — so neither arm
// can ever be promoted. They are price tags, not levers.
//
// WORKLOAD INVARIANT (17a law 22 — a corrupting ceiling probe is only a price with one).
// Deleting work in an incorrect-output arm can change the GUEST's control flow, and then the arm
// runs a different amount of downstream work and the "ceiling" is worthless. g492NoteEdge() counts,
// PER ARM and inside the same run, every entry into all five backend edges plus the batch draw
// count and the front end's CPU-replay fallback counter. `[G492:inv]` prints both arms beside every
// `[G419:ab]` window. The probe is admissible only when the non-target edges, the draw count and
// the fallback count all match across arms; the target edge's own `*Skip` column is the lever.
//
// SCOPE OF THE DELETION — deliberately narrower than the edge, to keep the invariant intact:
//   * norender skips submitImpl only when `depthReadback == nullptr`. readDepth() and G275's fused
//     render-then-read-depth hand the worker an OUT vector whose SIZE the front end then validates;
//     leaving it stale would flip a size check and change the caller's path, which is exactly the
//     failure law 22 warns about. `renderSkip` vs `render` reports the coverage achieved.
//   * noother skips the T8 view, the CT24 view and the two composite builders, and only when their
//     verifier out-params are not requested (`verify` / `readback` are default-off oracles).
//   * The G415 colour window is still consumed and cleared on the skip path. It is per-submit state:
//     leaving it published would leak it onto a LATER submit, i.e. a behaviour difference outside
//     the probe.
//
// Levers: DC2_G419_AB=norender / =noother (the only admissible gate; hold 16 — both are
// thread-level: they change when and whether the GL worker has anything queued).
// Standalone arms for a single-run look: DC2_G492_NO_RENDER=1 / DC2_G492_NO_OTHER=1.
//
// NOTE: g492NoRenderArm() / g492NoOtherArm() / g492NoteEdge() are defined at global scope in
// g419_ab_instrument.inc and declared in ps2_gs_gpu_raster.cpp ABOVE the anonymous namespace this
// part sits inside. Do not re-declare them here — a declaration inside the anon namespace binds to
// an internal-linkage symbol and fails to link (the appendix's anonymous-namespace linkage trap,
// which cost G491 one compile).

// Edge classes for g492NoteEdge(); same numbering as the [G447:edge] census so the two reports can
// be read side by side.
static constexpr int kG492EdgeRender = 0;
static constexpr int kG492EdgeReadColor = 1;
static constexpr int kG492EdgeWriteColor = 2;
static constexpr int kG492EdgeCopy = 3;
static constexpr int kG492EdgeOther = 4;

static bool g492NoRenderActive()
{
    const int arm = g492NoRenderArm(); // -1 unless DC2_G419_AB=norender
    if (arm >= 0)
        return arm == 1;
    static const bool on = (std::getenv("DC2_G492_NO_RENDER") != nullptr);
    return on;
}

static bool g492NoOtherActive()
{
    const int arm = g492NoOtherArm(); // -1 unless DC2_G419_AB=noother
    if (arm >= 0)
        return arm == 1;
    static const bool on = (std::getenv("DC2_G492_NO_OTHER") != nullptr);
    return on;
}

// True only while one of the two probes can actually fire. Keeps the per-edge invariant counting
// (one relaxed atomic add per backend item, ~73/frame) off the shipping path entirely.
static bool g492ProbeArmed()
{
    static const bool on = (g492NoRenderArm() >= 0) || (g492NoOtherArm() >= 0) ||
                           (std::getenv("DC2_G492_NO_RENDER") != nullptr) ||
                           (std::getenv("DC2_G492_NO_OTHER") != nullptr) ||
                           (std::getenv("DC2_G492_INV") != nullptr);
    return on;
}

static void g492Note(int edge, unsigned long long draws, bool skipped)
{
    if (g492ProbeArmed())
        g492NoteEdge(edge, draws, skipped ? 1ull : 0ull);
}

// ===== G492 LEVER — reuse the CT24 view's output texture storage ===============================
//
// WHERE THE CEILING POINTED. `noother` priced the whole backend-other bucket at -1.66/-1.93 ms/f
// and [G453:other] then split it by subtype on the same route:
//
//   [G453:other] t8=1.04-1.12/10.0  ct24=0.90-1.13/1.0  composite=0.97-1.05/9.0  upload=0.27-0.40/11.0
//   [G447:edge]  backend-other = 3.01-3.27 ms/f over 20.0  (caller wait ~= worker work: no
//                measurable handoff residue in this bucket — it is GL work, not latency)
//
// ONE CT24 view build per frame costs 0.90-1.13 ms — as much as all ten T8 builds together and
// ~10x the per-item cost of every other item in the bucket. That is not an overlap problem.
//
// ROOT. prepareCt24ViewOnWorker calls glTexImage2D(..., texW, texH, 0, RGBA, UNSIGNED_BYTE,
// nullptr) on the output view EVERY build, even when the cached entry already has that exact id,
// width, height and internal format. That is a full storage REALLOCATION of a texture the very
// next call attaches to m_g281Fbo as the colour attachment and renders into, i.e. the driver is
// asked to orphan and re-create an attachment's backing store once per frame.
//
// EXACT BY CONSTRUCTION. The build's draw is a single full-screen triangle under
// glViewport(0,0,texW,texH) with SCISSOR/DEPTH/BLEND disabled and a full glColorMask, so it writes
// EVERY texel of the output. No texel of the previous content can survive into the produced view,
// which is why dropping the reallocation cannot change a single output byte. Format, filtering,
// wrap mode, FBO attachment and draw order are all unchanged; only the redundant allocation goes.
// The reuse is gated on the ENTRY's own flag (TexEntry::g492Ct24Alloc), set only by this function,
// so an entry that was evicted, recreated, or built by any other path re-allocates normally.
//
// This is NOT G431. G431 reused the T8 build's MAP and CLUT upload storage (glTexSubImage2D
// instead of glTexImage2D on the two INPUT textures) and measured neutral — on a ~48 ms/f binary,
// pooled cross-run, i.e. expired by 17a law 7d and inadmissible by law 15a. This lever is a
// different texture (the OUTPUT view), a different subtype (ct24, not t8) and a different cost
// class (1 item at ~1 ms, not 10 at ~0.18 ms). G431's arm is untouched and stays opt-in.
//
// Levers: DC2_G419_AB=ct24reuse (paired gate), DC2_G492_CT24_REUSE=1 (standalone arm),
// DC2_G492_NO_CT24_REUSE=1 (rollback, wins over the arm).
// Oracle: the path's own DC2_G338_VERIFY=1 readback comparison — it recomputes every output texel
// from the source FBO on the CPU and counts disagreements, so `bad=0` under the arm IS the
// exactness proof.
static bool g492Ct24ReuseActive()
{
    const int arm = g492Ct24ReuseArm(); // -1 unless DC2_G419_AB=ct24reuse
    if (arm >= 0)
        return arm == 1;
    static const bool on = (std::getenv("DC2_G492_CT24_REUSE") != nullptr) &&
                           (std::getenv("DC2_G492_NO_CT24_REUSE") == nullptr);
    return on;
}
