// ===== G629: THE CPU BAND REPLAY AS A GPU-RESIDENT BATCHED BACKEND =====
// revision: 1   (DEFAULT-OFF in every arm; nothing here runs, allocates or branches unless armed)
//
// ---------------------------------------------------------------------------------------------
// THE MEASUREMENT THAT LED HERE (this phase, route `ridepod`, shipped G628 binary, 10,441 presents)
//
//   [G529:cls]   class   batches   pool wall (cum)   us/batch   entries/batch
//                discov   20,514      70,244.6 ms      3,424        1,457
//                legacy      338       2,854.3 ms      8,445        1,737
//                disp        348         679.7 ms      1,953          210
//                TOTAL    21,200      73,778.6 ms
//
//   [G529:disp]  wall=73,778.6  lane0=6,846.9  worker=252,907.1  rt=66,931.7  (cumMs)
//
// ⭐⭐⭐ Normalised: the CPU band replay is **7.07 ms/f of GS-worker wall** against a
// `gsWorkerMs/f` of 23.7 — 30% of the pole thread — and `discov` alone is 6.73 ms/f of it.
// ⭐⭐⭐ And it BURNS 24.9 ms/f of aggregate CPU (lane0 + worker) across 8 `GSRowPool` lanes to
// produce those 7.07 ms of wall: a parallel efficiency of **44%** on a 6-core host that is also
// running the EE thread (15.6 ms/f cpu), the VU1 thread (17.9 ms/f busy) and the GL driver.
// `rt = wall - lane0 = 6.41 ms/f` is the pole thread doing nothing but waiting.
//
// So this stage is not a 13% tax on the GS worker. It is the largest single CPU consumer in the
// process, and the thread it blocks is the one the frame is poled on.
//
// ---------------------------------------------------------------------------------------------
// WHY A GPU BACKEND, AND WHY *THIS* ONE
//
// ⛔ THREE PHASES ALREADY REFUTED THE CHEAP VERSION. G586, G587 and G604 each tried to move these
// populations onto the **generic GL fixed-function raster path** by widening `g178ClassifyEntry`'s
// admission. All three failed exactness with the same signature (G604 §2.4: 2.044% of 360 M px off
// by >1 LSB, max channel delta 255, 3.15 M private-Z mismatches, and pixels where the GPU writes
// BLACK over content the CPU keeps), and G587's conclusion — confirmed by G604 at population scale —
// is quoted here in full because it is this phase's design brief:
//
//   "Future work on these families needs a materially different exact execution/ownership design,
//    NOT another address admission."
//
// ⭐⭐⭐ AND ONE PHASE ALREADY BUILT THE DIFFERENT DESIGN, ON ONE HARDCODED ADDRESS. G570/G571 ship
// **default-ON** an exact GPU COMPUTE replay of the `0x139` shadow-volume batch: a tile-binned
// compute shader that re-evaluates the rasteriser's own edge/interpolation arithmetic with
// `precise`-qualified, explicitly decomposed float expressions, gated by a per-pixel oracle
// (`60 batches / 15,728,640 px, bad=0`). That result retires the single biggest risk in this design
// — **GLSL binary32 reproduces this host's MSVC float arithmetic bit-exactly when the expression
// tree is decomposed and every temporary is `precise`** — and it does so by measurement, not by
// argument.
//
// G629 is therefore not "another address admission". It is G570's proven mechanism generalised
// from one hardcoded untextured TRIFAN class to the recurring `discov` population that actually
// carries the wall, with the CPU band replay kept as BOTH the exactness oracle and the cold
// fail-closed fallback.
//
// ---------------------------------------------------------------------------------------------
// ⛔ THIS FILE IS THE COUNTER, AND IT COMES FIRST (g527_lineclip_refuted_by_its_own_census)
//
// The claimed mechanism is "a bounded number of GPU dispatches can execute the classes that carry
// the replay wall". Two numbers decide whether that is true, and NEITHER is knowable from the
// existing censuses:
//
//   1. What FRACTION OF THE POOL WALL sits in batches every one of whose entries the GPU kernel
//      could reproduce? Admission is WHOLE-BATCH by construction — GS semantics are order-dependent
//      per pixel, so a batch is replayed atomically or not at all — and `[G605:leaf]` reports
//      0.196% of `discov` triangles as `tri-gen` (not lowered). At 1,457 entries/batch a 0.196%
//      per-entry refusal rate would admit only e^-2.9 ≈ 5% of BATCHES if those entries were spread
//      uniformly. The per-entry rate is therefore not the answer; the per-BATCH rate is, and it has
//      to be measured.
//   2. Which CONJUNCT refuses the batches that are refused — as a BITMASK, never a first-fail
//      bucket. G628's law, bought at the cost of an entire wrong plan: when a rejection reason is a
//      CONJUNCTION, census the CONJUNCTS. A first-fail bucket cannot say whether one predicate or
//      nine block a population, nor whether the population is one shape or a zoo.
//
// `[G629:admit]` answers both, plus it books the POOL WALL of admitted vs refused batches, because
// entries are only a proxy for work while entry SIZE is constant and here it is not
// (g568_rank_fallback_by_entries_not_events; `[G603:admit]`'s own header records the `s03` case
// where display batches were 1.7% of entries and 79% of the spike).
//
// ARMS
//   DC2_G629_STAT=1   the census below. Default-OFF, zero cost otherwise. It runs AFTER the pool
//                     returns, so it cannot perturb the wall it books.
//
// ---------------------------------------------------------------------------------------------
// THE ADMISSION, STATED ONCE
//
// A batch is admissible when EVERY entry is one of the four kernels below and they all target one
// CT32 surface. The conjuncts restate G605's and G628's per-draw admissions (they cannot simply be
// called: those live inside `drawTriangle` and read live restored state, not a captured entry), so
// any drift between them shows up as a refusal, never as a wrong pixel — the GPU arm's output is
// only ever committed after the whole batch is admitted, and the oracle compares it against the CPU
// replay that would otherwise have run.
//
//   kind            what the GPU kernel runs
//   TRI_UNTEX       g609TriScanRowT<false, ZPRIV, ., .>  — Gouraud, no sampler
//   TRI_TEX_FST     g609TriScanRowT<true, true, true, true>   — T8 shared CLUT, u/v 12.4
//   TRI_TEX_STQ     g609TriScanRowT<true, true, false, true>  — T8 shared CLUT, s/t/q
//   SPR_UNTEX       the untextured sprite fill (the leading clear of every discov batch)
//
// Anything else refuses the BATCH. Fail closed, never fail silent.

namespace
{

static bool g629StatOn()
{
    static const bool on = envFlagEnabled("DC2_G629_STAT");
    return on;
}

// ---------------------------------------------------------------------------------------------
// ⛔⛔⛔ THE CEILING PROBE — KNOWINGLY WRONG PIXELS. NEVER PROMOTE. NEVER IN A SHIPPED BINARY.
//
// `DC2_G629_CEIL=1` SKIPS the CPU band replay entirely for every batch this file would admit. The
// target is simply left unwritten, so the output is wrong by construction and the run is worthless
// for anything but timing.
//
// ⭐ WHY IT IS BUILT BEFORE THE BACKEND, NOT AFTER. This port's standing law (G511; the pattern
// G528 used for `DC2_G528_NO_PUB`) is that a deletion probe on a CONSUMER edge BOUNDS every lever
// that could ever be built there. The GPU backend this file exists to justify is thousands of lines
// of marshalling and GLSL, and `17e`'s "pricing before building" section is explicit that a lever
// of that size must be priced first. The probe answers the only question that can kill the design:
//
//   if deleting 85.1% of the replay pool wall does not move the frame, then NO backend for this
//   population can, however exact or fast it is — and the phase closes with a refutation instead of
//   a shader that measures null.
//
// It is also the honest upper bound for the payoff section: a real GPU backend must pay its own
// dispatch, upload and readback, so it can only ever recover a FRACTION of what this probe deletes.
static bool g629CeilOn()
{
    static const bool on = envFlagEnabled("DC2_G629_CEIL");
    return on;
}

// ---------------------------------------------------------------------------------------------
// THE BACKEND ARMS. All default-OFF; `g629_gpu_kernel.inc` (included after G609, because the
// marshalling needs `g530RowAddr` / `g609*` / `g403DisplayZBuf`) holds the implementation. Declared
// here because the call site is `rasterizer_tilebin_capture.inc`, which the TU includes FIRST —
// the same forward-declaration pattern `g529_replay_dispatch.inc` already uses for G570/G580.
static bool g629GpuRequested();
static bool g629GpuVerifyOn();
// ⛔ These take a raw `vram` pointer and a palette CALLBACK rather than `GS*`/`GSRasterizer*`,
// because this is a free-function unit: only the flush closure (a member context) may touch GS's
// private members or call `lookupCLUT`. The callback handed in is the very `g605ClutFill` lambda
// the CPU arm already uses, so both arms decode the palette through one piece of code.
static bool g629TryReplayGpu(std::vector<G144Entry> &list, GS *gs, uint8_t *vram,
                             uint32_t vramSize, const G580ClutFillFn &clutFill);
static void g629VerifyAfterCpu(uint8_t *vram);

// ---------------------------------------------------------------------------------------------
// Entry kinds and the per-conjunct refusal bits.

enum G629Kind
{
    kG629KindTriUntex = 0,
    kG629KindTriTexFst,
    kG629KindTriTexStq,
    kG629KindSprUntex,
    kG629KindOther, // refused
    kG629KindN
};
static const char *const kG629KindName[kG629KindN] = {
    "tri-untex", "tri-tex-fst", "tri-tex-stq", "spr-untex", "OTHER"};

// One bit per INDEPENDENT conjunct. Never a first-fail bucket (G628's law).
enum G629Bit
{
    kG629BitPrim = 0,   // primitive class outside {TRIANGLE, TRISTRIP, TRIFAN, SPRITE}
    kG629BitTarget,     // fbp/fbw/psm differs from entry 0, or psm != CT32
    kG629BitFbmsk,      // frame.fbmsk != 0
    kG629BitFba,        // ctx.fba != 0
    kG629BitPabe,       // per-pixel alpha blending
    kG629BitIip,        // triangle without Gouraud (the kernels interpolate unconditionally)
    kG629BitAa1,        // anti-aliased edge
    kG629BitAtest,      // alpha test does not always pass
    kG629BitTexPsm,     // textured, but not the T8 shared-CLUT shape
    kG629BitTexMode,    // textured, but tfx != MODULATE or tcc != 1 or tbw == 0
    kG629BitClutFmt,    // CLUT not CT32/CSM0/CSA0
    kG629BitFilter,     // textured, but not the BILINEAR shape the kernel implements
    kG629BitDepthPriv,  // ⭐ depth is REAL guest-VRAM Z (neither the PRIV mirror nor dead)
    kG629BitBounds,     // bbox outside the private-Z window
    kG629BitSprTex,     // textured sprite (not yet a kernel)
    kG629BitLine,       // line primitive
    kG629Bit139,        // fbp 0x139 — G570/G571 already owns this class
    kG629BitDiag,       // a per-pixel diagnostic / legacy-Z override is armed process-wide
    kG629BitN
};
static const char *const kG629BitName[kG629BitN] = {
    "prim", "target", "fbmsk", "fba", "pabe", "iip", "aa1", "atest",
    "texpsm", "texmode", "clutfmt", "filter", "depthpriv", "bounds", "sprtex",
    "line", "fbp139", "diag"};

// ---------------------------------------------------------------------------------------------
// ⛔ THE PROCESS-WIDE QUIET PREDICATE, CACHED AT NAMESPACE SCOPE.
// Every one of these gates a per-pixel branch the GPU kernel does NOT reproduce. They are all
// env-driven and constant for the process, so they are read once — a function-local
// `static const bool` here would be an MSVC per-access TLS guard (`_Init_thread_epoch`), which is
// the exact defect `PS2_PROJECT_STATE.md`'s "Diagnostic Hygiene in Hot Loops" rule names and which
// G626 measured as a NULL lever after the fact.
static const bool g_g629DiagQuiet =
    // G365's sprite ceil-edge rule is the DEFAULT and is what `g629BuildRecords` reproduces for
    // the batch's leading clear. Disarming it switches `drawSprite` to a truncated-corner rect, so
    // a run with it set must refuse rather than replay a rect the CPU no longer uses.
    (std::getenv("DC2_G365_NO_SPRITE_EDGE") == nullptr) &&
    (std::getenv("DC2_G232_RTTDUMP") == nullptr) &&
    (std::getenv("DC2_G357_FLAT") == nullptr) &&
    (std::getenv("DC2_G404_LEGACY_DISPLAY_Z_SCOPE") == nullptr) &&
    (std::getenv("DC2_G408_LEGACY_SHADOW_COVERAGE") == nullptr);

// The G403/G411 private mirror window, restated so this file does not depend on include order.
// (`kG403DisplayZW` / `kG403DisplayZH` are the authority; asserted equal at first use.)
static uint32_t g629PrivZW() { return kG403DisplayZW; }
static uint32_t g629PrivZH() { return kG403DisplayZH; }

struct G629EntryFacts
{
    uint32_t kind = kG629KindOther;
    uint32_t bits = 0u; // failing conjuncts; 0 == admitted
    bool privZ = false; // runs the PRIV depth contract (false == depth is DEAD)
    bool zWrite = false;
    uint64_t texKey = 0ull;  // distinct sampled textures per batch — the upload driver
    uint64_t clutKey = 0ull; // distinct palettes per batch
    uint32_t texW = 0u, texH = 0u;
    uint8_t wrapU = 0u, wrapV = 0u;
};

// ---------------------------------------------------------------------------------------------
// ⛔⛔ THE ALPHA TEST — AND THE SECOND TIME THIS FILE'S OWN RESTATEMENT WAS THE DEFECT.
//
// The first version of this predicate read "ATE==0 or ATST==ALWAYS", which is the obvious reading
// and is WRONG. `[G629:bits]` then reported, over the whole `discov` population and with no other
// bit set anywhere:
//
//   [G629:bits] cls=discov fbp=141 entryFail(atest=2273030) batchFail(atest=1955)
//   [G629:bits] cls=discov fbp=142 entryFail(atest=1521736) batchFail(atest=772)
//   [G629:bits] cls=discov fbp=15d entryFail(atest=441232)  batchFail(atest=704)
//
// ONE bit, 100% of entries, every row. The population is `test=0x5000b` — ATE=1, **ATST=GEQUAL,
// AREF=0** — i.e. `alpha >= 0`, which every 8-bit alpha satisfies. It is a no-op alpha test that
// is not spelled ALWAYS, and G605 already knew this: its own predicate does NOT pattern-match the
// ATST field, it runs the SHIPPED `classifyAlphaTest` over all 256 alpha values and admits only if
// every one is a full unconditional write.
//
// ⭐ THE LESSON, AND IT IS THE SAME ONE TWICE (see the depth note below): do not RESTATE a shipped
// predicate — CALL it. Both defects in this file were restatements that looked correct and refused
// (or would have admitted) an entire population. `classifyAlphaTest` lives in
// `rasterizer_row_pool.inc`, which precedes this file in the TU, so the exhaustive form is
// reachable here verbatim; `g605AlphaTestAlwaysPasses` itself is not (it lives in
// `g605_tri_span.inc`, included later), so this is that function's body, not a paraphrase of it.
static bool g629AlphaAlwaysPassesUncached(uint64_t test)
{
    for (int a = 0; a < 256; ++a)
    {
        const AlphaTestResult r = classifyAlphaTest(test, static_cast<uint8_t>(a));
        if (!r.writeFramebuffer || !r.writeDepth || r.preserveDestinationAlpha)
            return false;
    }
    return true;
}

static bool g629AlphaAlwaysPasses(uint64_t test)
{
    // Memoised per distinct TEST value (G530's pattern). The census walks every entry of every
    // batch, so a 256-iteration loop per entry would be a self-inflating census.
    static thread_local uint64_t s_key[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    static thread_local bool s_val[4] = {false, false, false, false};
    static thread_local int s_n = 0;
    for (int i = 0; i < 4; ++i)
        if (s_key[i] == test)
            return s_val[i];
    const bool v = g629AlphaAlwaysPassesUncached(test);
    s_key[s_n & 3] = test;
    s_val[s_n & 3] = v;
    ++s_n;
    return v;
}

static G629EntryFacts g629ClassifyEntry(const G144Entry &e, const G144Entry &e0)
{
    G629EntryFacts f{};
    const GSContext &ctx = e.ctx;
    const uint32_t prim = static_cast<uint32_t>(e.prim.type);

    const bool isTri = (prim == GS_PRIM_TRIANGLE || prim == GS_PRIM_TRISTRIP ||
                        prim == GS_PRIM_TRIFAN);
    const bool isSpr = (prim == GS_PRIM_SPRITE);
    const bool isLine = (prim == GS_PRIM_LINE || prim == GS_PRIM_LINESTRIP);
    if (isLine)
        f.bits |= 1u << kG629BitLine;
    else if (!isTri && !isSpr)
        f.bits |= 1u << kG629BitPrim;

    if (ctx.frame.psm != GS_PSM_CT32 || ctx.frame.fbp != e0.ctx.frame.fbp ||
        ctx.frame.fbw != e0.ctx.frame.fbw || ctx.frame.fbw == 0u)
        f.bits |= 1u << kG629BitTarget;
    // 0x139 is G570/G571's promoted exact-GPU class. Refusing it here keeps the two mechanisms
    // disjoint, so neither becomes a second architectural variable in the other's gate.
    if (ctx.frame.fbp == 0x139u)
        f.bits |= 1u << kG629Bit139;
    if (ctx.frame.fbmsk != 0u)
        f.bits |= 1u << kG629BitFbmsk;
    if (ctx.fba != 0u)
        f.bits |= 1u << kG629BitFba;
    if (e.pabe)
        f.bits |= 1u << kG629BitPabe;
    if (e.prim.aa1)
        f.bits |= 1u << kG629BitAa1;
    if (!g629AlphaAlwaysPasses(ctx.test))
        f.bits |= 1u << kG629BitAtest;
    if (isTri && !e.prim.iip)
        f.bits |= 1u << kG629BitIip;
    if (!g_g629DiagQuiet || !g203UniversalZEnabled())
        f.bits |= 1u << kG629BitDiag;

    if (e.prim.tme)
    {
        if (isSpr)
            f.bits |= 1u << kG629BitSprTex;
        if (ctx.tex0.psm != GS_PSM_T8)
            f.bits |= 1u << kG629BitTexPsm;
        if (ctx.tex0.tfx != 0u || ctx.tex0.tcc != 1u || ctx.tex0.tbw == 0u)
            f.bits |= 1u << kG629BitTexMode;
        if (ctx.tex0.cpsm != GS_PSM_CT32 || ctx.tex0.csm != 0u || ctx.tex0.csa != 0u)
            f.bits |= 1u << kG629BitClutFmt;
        // The kernel's sampler is `g609SampleT8`, which is BILINEAR unconditionally. G605's own
        // admission carries this as `g141Linear`; restated here, on the captured `tex1`.
        if (!tex1UsesLinearFilter(ctx.tex1))
            f.bits |= 1u << kG629BitFilter;
        f.texKey = (static_cast<uint64_t>(ctx.tex0.tbp0) << 24) |
                   (static_cast<uint64_t>(ctx.tex0.tbw) << 16) |
                   (static_cast<uint64_t>(ctx.tex0.tw) << 8) |
                   static_cast<uint64_t>(ctx.tex0.th);
        f.clutKey = (static_cast<uint64_t>(ctx.tex0.cbp) << 32) |
                    (static_cast<uint64_t>(ctx.tex0.csa) << 24) |
                    (static_cast<uint64_t>(e.texclut.cbw) << 16) |
                    (static_cast<uint64_t>(e.texclut.cou) << 8) |
                    static_cast<uint64_t>(e.texclut.cov);
        f.texW = 1u << (ctx.tex0.tw & 0xFu);
        f.texH = 1u << (ctx.tex0.th & 0xFu);
        f.wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
        f.wrapV = static_cast<uint8_t>((ctx.clamp >> 2) & 0x3u);
    }

    // ---- depth ----
    // ⛔ THE PREDICATE THAT MUST MATCH `drawTriangle`, AND A TRAP THIS FILE FELL INTO FIRST.
    // The generic path composes `zScope = g203Uni || fbp==0x139 || g106TitleZ || g202TownZ` and
    // `zEnabled = zScope && zte`. `g404SharedZScope` is a DIFFERENT predicate — it selects the
    // G403/G411 PRIVATE MIRROR (and requires `fbw == 8`). Deriving `zEnabled` from it, as this file
    // originally did, makes every `fbw != 8` GEQUAL depth-WRITING draw look like `zEnabled == false`
    // and therefore "dead depth" — which would silently admit ~1.04 M depth-writing entries per run
    // at `fbp=0x141 fbw=2` into a kernel with no depth buffer. `[G603:site]` names exactly that
    // population (`depth=1042527`). Read the generic path's own predicate, never a lookalike.
    const uint32_t zte = static_cast<uint32_t>((ctx.test >> 16) & 0x1u);
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 0x3u);
    const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
    const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
    const bool zmsk = ((ctx.zbuf >> 32) & 0x1u) != 0u;
    const bool z24 = (zpsm == 0x1u);
    // `kG629BitDiag` already refuses the batch unless `g203UniversalZEnabled()`, which is the term
    // that makes `zScope` unconditionally true — so the legacy disjuncts cannot change the answer
    // for any batch this file admits.
    const bool zScope = g203UniversalZEnabled();
    const bool zEnabled = zScope && zte != 0u;
    // `zWrite` is the generic path's OWN predicate. Required, not assumed: a ZTST=ALWAYS draw that
    // DOES write depth must fail closed rather than silently lose its Z write (G628 §3c).
    const bool zWrite = zEnabled && !zmsk && ztst != 0u;
    const bool privMirror = zScope && g404SharedZScope(ctx.frame.fbp, ctx.frame.fbw, zbpPage, zpsm);
    const bool depthPrivOk = zEnabled && z24 && ztst == 2u && zbpPage == 0xd0u && privMirror;
    const bool depthDeadOk = (!zEnabled || ztst == 1u) && !zWrite;
    if (!depthPrivOk && !depthDeadOk)
        f.bits |= 1u << kG629BitDepthPriv;
    f.privZ = depthPrivOk;
    f.zWrite = zWrite;

    // ---- bounds: the kernel commits through the private-Z window's addressing ----
    int minX = 0x7fffffff, maxX = -0x7fffffff;
    const int nv = isSpr ? 2 : 3;
    for (int i = 0; i < nv; ++i)
    {
        const int xi = static_cast<int>(e.v[i].x);
        if (xi < minX) minX = xi;
        if (xi > maxX) maxX = xi;
    }
    if (minX < static_cast<int>(ctx.scissor.x0)) minX = static_cast<int>(ctx.scissor.x0);
    if (maxX > static_cast<int>(ctx.scissor.x1)) maxX = static_cast<int>(ctx.scissor.x1);
    if (!(e.minY >= 0 && e.maxY < static_cast<int>(g629PrivZH()) &&
          minX >= 0 && maxX < static_cast<int>(g629PrivZW())))
        f.bits |= 1u << kG629BitBounds;

    if (f.bits == 0u)
    {
        if (isSpr)
            f.kind = kG629KindSprUntex;
        else if (!e.prim.tme)
            f.kind = kG629KindTriUntex;
        else
            f.kind = e.prim.fst ? kG629KindTriTexFst : kG629KindTriTexStq;
    }
    return f;
}

// ---------------------------------------------------------------------------------------------
// The census. thread_local + non-atomic: the flush closure runs on ONE thread (the G144 flush-owner
// contract every function in it already relies on).

struct G629Row
{
    uint64_t batches = 0, entries = 0;
    uint64_t okBatches = 0, okEntries = 0;
    uint64_t wallNs = 0, okWallNs = 0;
    uint64_t kind[kG629KindN] = {};
    uint64_t bit[kG629BitN] = {};        // per-ENTRY failing conjuncts
    uint64_t batchBit[kG629BitN] = {};   // per-BATCH: this conjunct refused at least one entry
    uint64_t maxEntries = 0;
    uint64_t rowsMax = 0;
    uint32_t fbwSeen = 0;
    // ⭐ THE DISPATCH SIZING TERMS. A GPU replay of a batch must make every sampled texture and
    // palette visible to the shader before it runs, so the number of DISTINCT ones per batch — not
    // the entry count — is what decides whether one bounded dispatch can carry the batch or whether
    // the design degenerates into per-entry uploads (which is `kickall`'s refuted shape).
    uint64_t texKeysSum = 0, texKeysMax = 0;
    uint64_t clutKeysSum = 0, clutKeysMax = 0;
    uint64_t texBatches = 0; // batches that sampled anything at all
    // ⭐ THE UPLOAD BUDGET. The shader samples a CPU-decoded linear `clut[ReadP8(...)]` image per
    // distinct texture, so the batch's total decoded TEXELS is the per-batch upload — and a design
    // that assumed "a few small textures" and met 1024x1024 ones would move the cost rather than
    // delete it. Measured, not assumed.
    uint64_t texelsSum = 0, texelsMax = 0;
    uint32_t texWMax = 0, texHMax = 0;
    uint64_t wrapMode[4] = {}; // 0 REPEAT, 1 CLAMP, 2 REGION_CLAMP, 3 REGION_REPEAT
};

// key = cls << 16 | fbw << 9 | fbp
//
// ⚠️ FBW IS PART OF THE KEY, AND `[G603:row]` SHOWS WHY. That census keys on `(cls, fbp)` alone and
// stores `fbwSeen` as a scalar, so `fbp=0x141` — which this route drives at **both** `fbw=2` and
// `fbw=8` — prints as one row whose width field simply changes between reports. Two different
// surface geometries were being summed into one denominator. They are separate populations for a
// replay backend (the CT32 page stride differs), so they are separate rows here.
static std::map<uint32_t, G629Row> g_g629Rows;
static uint64_t g_g629Batches = 0ull;

// Batch-shape histogram: how many DISTINCT refusal masks the refused batches carry. `rows=N
// overflow=0` is what proves the refused population is one shape rather than a zoo (G628).
static const int kG629MaskRows = 24;
static uint32_t g_g629MaskKey[kG629MaskRows];
static uint64_t g_g629MaskN[kG629MaskRows];
static uint64_t g_g629MaskEntries[kG629MaskRows];
static uint64_t g_g629MaskWallNs[kG629MaskRows];
static int g_g629MaskUsed = 0;
static uint64_t g_g629MaskOverflow = 0ull;

static void g629NoteMask(uint32_t mask, uint64_t entries, uint64_t wallNs)
{
    for (int i = 0; i < g_g629MaskUsed; ++i)
    {
        if (g_g629MaskKey[i] == mask)
        {
            ++g_g629MaskN[i];
            g_g629MaskEntries[i] += entries;
            g_g629MaskWallNs[i] += wallNs;
            return;
        }
    }
    if (g_g629MaskUsed < kG629MaskRows)
    {
        const int i = g_g629MaskUsed++;
        g_g629MaskKey[i] = mask;
        g_g629MaskN[i] = 1u;
        g_g629MaskEntries[i] = entries;
        g_g629MaskWallNs[i] = wallNs;
        return;
    }
    ++g_g629MaskOverflow;
}

static void g629FormatMask(uint32_t mask, char *buf, size_t cap)
{
    size_t n = 0;
    buf[0] = '\0';
    if (mask == 0u)
    {
        std::snprintf(buf, cap, "ADMITTED");
        return;
    }
    for (int b = 0; b < kG629BitN && n + 12u < cap; ++b)
    {
        if ((mask & (1u << b)) == 0u)
            continue;
        n += static_cast<size_t>(
            std::snprintf(buf + n, cap - n, "%s%s", (n == 0u) ? "" : ",", kG629BitName[b]));
    }
}

static void g629Report()
{
    uint64_t totB = 0, totOkB = 0, totE = 0, totOkE = 0, totW = 0, totOkW = 0;
    for (const auto &kv : g_g629Rows)
    {
        totB += kv.second.batches;   totOkB += kv.second.okBatches;
        totE += kv.second.entries;   totOkE += kv.second.okEntries;
        totW += kv.second.wallNs;    totOkW += kv.second.okWallNs;
    }
    // ⭐ The WALL share is the headline, not the batch share: entries and batches are proxies for
    // the work only while their SIZE is constant, and here it spans three orders of magnitude.
    std::fprintf(stderr,
                 "[G629:admit] batches=%llu ok=%llu (%.1f%%) entries=%llu ok=%llu (%.1f%%) "
                 "poolWall=%.1fms okWall=%.1fms (%.1f%%) rows=%zu\n",
                 (unsigned long long)totB, (unsigned long long)totOkB,
                 totB ? 100.0 * (double)totOkB / (double)totB : 0.0,
                 (unsigned long long)totE, (unsigned long long)totOkE,
                 totE ? 100.0 * (double)totOkE / (double)totE : 0.0,
                 (double)totW / 1e6, (double)totOkW / 1e6,
                 totW ? 100.0 * (double)totOkW / (double)totW : 0.0,
                 g_g629Rows.size());
    for (const auto &kv : g_g629Rows)
    {
        const uint32_t cls = kv.first >> 16;
        const uint32_t fbp = kv.first & 0x1FFu;
        const G629Row &r = kv.second;
        std::fprintf(stderr,
                     "[G629:row] cls=%-6s fbp=%03x fbw=%u batches=%llu ok=%llu entries=%llu "
                     "ok=%llu wall=%.1fms okWall=%.1fms maxEntries=%llu rowsMax=%llu "
                     "tex/b=%.2f texMax=%llu clut/b=%.2f clutMax=%llu\n",
                     kG528ClsName[cls & 3u], fbp, r.fbwSeen,
                     (unsigned long long)r.batches, (unsigned long long)r.okBatches,
                     (unsigned long long)r.entries, (unsigned long long)r.okEntries,
                     (double)r.wallNs / 1e6, (double)r.okWallNs / 1e6,
                     (unsigned long long)r.maxEntries, (unsigned long long)r.rowsMax,
                     r.texBatches ? (double)r.texKeysSum / (double)r.texBatches : 0.0,
                     (unsigned long long)r.texKeysMax,
                     r.texBatches ? (double)r.clutKeysSum / (double)r.texBatches : 0.0,
                     (unsigned long long)r.clutKeysMax);
        std::fprintf(stderr,
                     "[G629:tex] cls=%-6s fbp=%03x texW<=%u texH<=%u texels/b=%.0f texelsMax=%llu "
                     "(uploadMax=%.0fKB) wrap(rep=%llu clamp=%llu rgnclamp=%llu rgnrep=%llu)\n",
                     kG528ClsName[cls & 3u], fbp, r.texWMax, r.texHMax,
                     r.texBatches ? (double)r.texelsSum / (double)r.texBatches : 0.0,
                     (unsigned long long)r.texelsMax, (double)r.texelsMax * 4.0 / 1024.0,
                     (unsigned long long)r.wrapMode[0], (unsigned long long)r.wrapMode[1],
                     (unsigned long long)r.wrapMode[2], (unsigned long long)r.wrapMode[3]);
        std::fprintf(stderr, "[G629:kind] cls=%-6s fbp=%03x", kG528ClsName[cls & 3u], fbp);
        for (int k = 0; k < kG629KindN; ++k)
            if (r.kind[k])
                std::fprintf(stderr, " %s=%llu", kG629KindName[k],
                             (unsigned long long)r.kind[k]);
        std::fprintf(stderr, "\n[G629:bits] cls=%-6s fbp=%03x entryFail(", kG528ClsName[cls & 3u], fbp);
        for (int b = 0; b < kG629BitN; ++b)
            if (r.bit[b])
                std::fprintf(stderr, "%s=%llu ", kG629BitName[b], (unsigned long long)r.bit[b]);
        std::fprintf(stderr, ") batchFail(");
        for (int b = 0; b < kG629BitN; ++b)
            if (r.batchBit[b])
                std::fprintf(stderr, "%s=%llu ", kG629BitName[b], (unsigned long long)r.batchBit[b]);
        std::fprintf(stderr, ")\n");
    }
    std::fprintf(stderr, "[G629:mask] rows=%d overflow=%llu\n", g_g629MaskUsed,
                 (unsigned long long)g_g629MaskOverflow);
    for (int i = 0; i < g_g629MaskUsed; ++i)
    {
        char buf[256];
        g629FormatMask(g_g629MaskKey[i], buf, sizeof(buf));
        std::fprintf(stderr, "[G629:mask] #%02d n=%llu entries=%llu wall=%.1fms fail={%s}\n",
                     i, (unsigned long long)g_g629MaskN[i],
                     (unsigned long long)g_g629MaskEntries[i],
                     (double)g_g629MaskWallNs[i] / 1e6, buf);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// THE ADMISSION, as one predicate. Used by the ceiling probe now and by the backend later, so the
// thing being PRICED and the thing being BUILT can never diverge — the failure mode G604 §2.1 hit
// when the roadmap's Target 1 was scoped from a census of a different predicate.
//
// Whole-batch by construction: GS blending and depth are order-dependent per pixel, so a batch is
// replayed atomically or not at all. Any single refused entry refuses the batch.
static bool g629BatchAdmissible(const std::vector<G144Entry> &list)
{
    if (list.empty())
        return false;
    const G144Entry &e0 = list[0];
    for (const G144Entry &e : list)
        if (g629ClassifyEntry(e, e0).bits != 0u)
            return false;
    return true;
}

// ⛔ Ceiling-probe gate + its own counter. A probe with no path counter cannot prove its arms
// diverged (`g538_null_ab_needs_a_path_counter`), and a ceiling that silently never fired would
// read as "the lever is worth nothing" — the most expensive possible wrong answer here.
static uint64_t g_g629CeilSkipped = 0ull;
static uint64_t g_g629CeilEntries = 0ull;
static uint64_t g_g629CeilKept = 0ull;

static bool g629CeilSkipBatch(const std::vector<G144Entry> &list)
{
    if (!g629CeilOn())
        return false;
    if (!g629BatchAdmissible(list))
    {
        ++g_g629CeilKept;
        return false;
    }
    ++g_g629CeilSkipped;
    g_g629CeilEntries += list.size();
    if ((g_g629CeilSkipped % 512ull) == 0ull)
        std::fprintf(stderr,
                     "[G629:ceil] ⛔WRONG-PIXELS skipped=%llu entries=%llu kept=%llu\n",
                     (unsigned long long)g_g629CeilSkipped,
                     (unsigned long long)g_g629CeilEntries,
                     (unsigned long long)g_g629CeilKept);
    return true;
}

// The single hook, called from the flush closure AFTER `GSRowPool::run` returns so it cannot
// perturb the wall it books. `wallNs` is the caller-observed pool wall G529 already measures.
static void g629NoteBatch(const std::vector<G144Entry> &list, uint64_t wallNs, uint64_t rows)
{
    if (!g629StatOn() || list.empty())
        return;
    const G144Entry &e0 = list[0];
    const uint32_t fbp = e0.ctx.frame.fbp & 0x1FFu;
    const uint32_t cls = g528ClassifyFbp(e0.ctx.frame.fbp);
    G629Row &r = g_g629Rows[(cls << 16) | ((e0.ctx.frame.fbw & 0x7Fu) << 9) | fbp];
    ++r.batches;
    r.entries += list.size();
    r.wallNs += wallNs;
    r.fbwSeen = e0.ctx.frame.fbw;
    if (list.size() > r.maxEntries)
        r.maxEntries = list.size();
    if (rows > r.rowsMax)
        r.rowsMax = rows;

    uint32_t batchMask = 0u;
    // Small linear sets: batches carry a handful of distinct textures, and a linear scan over <=64
    // keys per entry is cheaper than a hash here AND is bounded, so the census cannot blow up on a
    // pathological batch. Overflow is reported rather than silently truncated.
    uint64_t texSet[64], clutSet[64];
    int nTex = 0, nClut = 0;
    bool setOverflow = false;
    uint64_t texelsThisBatch = 0ull;
    for (const G144Entry &e : list)
    {
        const G629EntryFacts f = g629ClassifyEntry(e, e0);
        ++r.kind[f.kind];
        batchMask |= f.bits;
        for (int b = 0; b < kG629BitN; ++b)
            if (f.bits & (1u << b))
                ++r.bit[b];
        if (f.texKey != 0ull)
        {
            ++r.wrapMode[f.wrapU & 3u];
            ++r.wrapMode[f.wrapV & 3u];
            if (f.texW > r.texWMax) r.texWMax = f.texW;
            if (f.texH > r.texHMax) r.texHMax = f.texH;
            int i = 0;
            for (; i < nTex; ++i)
                if (texSet[i] == f.texKey) break;
            if (i == nTex)
            {
                if (nTex < 64) { texSet[nTex] = f.texKey;
                                 texelsThisBatch += (uint64_t)f.texW * (uint64_t)f.texH; ++nTex; }
                else setOverflow = true;
            }
            int j = 0;
            for (; j < nClut; ++j)
                if (clutSet[j] == f.clutKey) break;
            if (j == nClut) { if (nClut < 64) clutSet[nClut++] = f.clutKey; else setOverflow = true; }
        }
    }
    if (nTex > 0)
    {
        ++r.texBatches;
        r.texKeysSum += static_cast<uint64_t>(nTex);
        r.clutKeysSum += static_cast<uint64_t>(nClut);
        if (static_cast<uint64_t>(nTex) > r.texKeysMax)
            r.texKeysMax = static_cast<uint64_t>(nTex);
        if (static_cast<uint64_t>(nClut) > r.clutKeysMax)
            r.clutKeysMax = static_cast<uint64_t>(nClut);
        r.texelsSum += texelsThisBatch;
        if (texelsThisBatch > r.texelsMax)
            r.texelsMax = texelsThisBatch;
        if (setOverflow)
            r.texKeysMax = 64u; // reported as saturated; see [G629:row] texMax
    }
    for (int b = 0; b < kG629BitN; ++b)
        if (batchMask & (1u << b))
            ++r.batchBit[b];
    if (batchMask == 0u)
    {
        ++r.okBatches;
        r.okEntries += list.size();
        r.okWallNs += wallNs;
    }
    g629NoteMask(batchMask, list.size(), wallNs);
    if ((++g_g629Batches % 2048ull) == 0ull)
        g629Report();
}

} // namespace
