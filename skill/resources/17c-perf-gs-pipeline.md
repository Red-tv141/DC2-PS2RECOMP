# Reference: GS Pipeline Performance Playbooks — Rasterizer, GPU LLE, Native Renderer

> **Load this only once measurement has proved the GS side is the pole** (`17a-perf-measurement.md`
> §A — a derivative probe, not an occupancy timer). These are the three big architectural levers on
> the GS path, each with the correctness contract that makes it promotable. They are subsystem-
> specific by nature; the cross-subsystem method is in `17a`/`17b`.

---

## §1 Parallelizing the GS Software Rasterizer — the Big Lever + its Correctness Contract

The rasterizer is usually the #1 cost and is embarrassingly parallel at the PIXEL level, so it is the
one place multi-threading is a *sanctioned* optimization (unlike guest execution — §4). Two tiers,
in payoff order:

1. **Eliminate/hoist first (bit-exact, no threads).** Pull every per-triangle-invariant out of the
   per-pixel loop: texture-sampler setup, CLUT decode (memoize into a per-triangle table — for a
   *bilinear* paletted texture that is 4 lookups/pixel collapsed to ≤256 decodes/triangle),
   swizzle-address base, alpha-test/blend decode; inline the sampler fast-path for the dominant PSM.
   Pure wins, zero concurrency risk. Do these before ANY threading — and measure: micro-caching the
   sampler can be *diminishing* if the real cost is the raw swizzled VRAM texel reads, not the decode.

2. **Then parallelize across disjoint pixels.** *Row-within-a-triangle* threading is the easy version
   but **plateaus (~1.2×)**: most triangles are small, so per-triangle dispatch/barrier overhead
   swamps the gain (Amdahl — the parallel fraction per triangle is tiny; a *high* size threshold
   makes it worse by threading almost nothing). The real win is **frame-level binning**: defer the
   frame's primitives into a display list, then let each worker own a disjoint screen BAND/TILE and
   replay the whole list clipped to its region — ONE dispatch for thousands of primitives, huge
   work/barrier ratio. On a real title this beat the row version ~3× (+11% → +32% fps).

**Correctness contract (bit-exact BY CONSTRUCTION, not by luck):**
- **Disjoint pixels per thread** (bands = disjoint scanline ranges ⇒ disjoint framebuffer/Z writes).
  Two threads must never touch the same pixel.
- **Preserve submission order WITHIN each thread's region** (replay entries in list order) so
  Z/alpha-blend output is identical to serial.
- **Per-thread scratch**: any per-primitive mutable cache (CLUT table, texel quad) becomes
  `thread_local`; per-pixel counters become per-lane and are summed after.
- **Snapshot the COMPLETE draw state** each deferred primitive reads (active context / prim / texture
  / CLUT / alpha / texa / 3 vertices) — a single missing field renders wrong *silently*. Enumerate
  every `gs->` field the pixel path touches before trusting the snapshot.
- **Flush barriers — the subtle, expensive-to-learn one:** deferral is safe only until something
  MUTATES state a deferred primitive depends on. **Any VRAM write between capture and replay is a
  flush barrier** — texture uploads (BITBLT / host-to-local / local-to-local), CLUT reloads,
  render-to-texture. These usually bypass the draw path, so hook them explicitly and drain the list
  before them. Deceptive symptom: the primitives drawn *before* a mid-frame re-upload look perfect
  while the ones whose texture got overwritten corrupt — it reads like "some geometry is wrong,"
  not like a texture bug. Include GPU readback, runtime/stub clears, direct `GSMem`/VRAM helper
  writes, legacy workarounds, and every path that bypasses the normal draw/GIF choke point in the
  writer inventory. Drain before each mutation or queue it through the same ordered FIFO; never
  apply it immediately from another thread against the worker's live GS context.
- **Selective dirty flushes are a second-phase optimization, not the first fix.** First prove
  unconditional upload barriers are correct. Only then replace them with a conservative overlap
  test. The dirty range must cover upload destination, local-to-local source, pending texture base,
  CLUT base, and render target. Unknown PSMs, wrap/alias uncertainty, or unsupported transfer modes
  must fail closed by flushing. Keep the selective path opt-in with a kill switch until title and
  dungeon soaks are clean.
- **Thread-context sensitivity:** a worker pool that is safe when driven from active-rendering code
  can *crash* (silent `std::terminate`/access-violation) when driven from the frame-end /
  present-adjacent path, where it races the host present thread. Keep the pool dispatch on the
  rendering thread; if you must drain a trailing tail at frame end, do it single-threaded.

**Verification when the scene animates:** pose/pan/water motion makes a byte-hash golden invalid
run-to-run — gate on the **nonzero-pixel COUNT** (±small noise) + a **visual A/B** + a **same-run
per-pixel sampler-verify**, and keep the whole thing behind a **default-OFF env** until a soak across
*every* scene (not just the easy one) is clean. Ship the parallel path opt-in first; promote to
default only after the soak.

---

## §2 Moving the Software Rasterizer to the GPU (LLE) — the Endgame Lever + its Contracts

When CPU-side parallelism plateaus (§3.1) and the profile says rasterization/sampling still
dominates, the remaining big win is rendering the deferred display list on a real GPU. A DC2
instance landed first-try clean: default → +50% GPU alone, ~2.5× combined with frame pipelining,
reaching the measured EE-bound ceiling. The contracts that made it work:

1. **Census before shader.** Log the DISTINCT render-state keys the live deferred list actually
   contains (prim/tfx/psm/blend A-B-C-D-FIX/ztst/clamp/filter/fbmsk/alpha-test incl. AREF) for a
   real scene BEFORE writing any GL code. Real workloads collapse to a tiny matrix (DC2 title:
   38 raw keys → 2 blend equations, alpha-test structurally always-pass because AREF=0, one Z
   mode). Build only that; count + fall back on the rest.
2. **CPU-PARITY, not spec-correctness, is the phase-1 contract.** The verified baseline is the
   shipped CPU rasterizer, including its non-hardware conventions: pixel centers, UV edge bias,
   and especially interpolation. A software sampler often interpolates the PRE-DIVIDED texture
   coordinate (affine `interp(s·1/|q|)` — GS ST arrives Q-premultiplied); a "correct"
   perspective-reconstructing shader then samples ~q× off-scale → wrap-noise / near-black 3D
   while 2D (UV/FST) looks perfect. Match the CPU (GLSL `noperspective` on ALL varyings, same
   pre-divide); upgrade quality later as a deliberate, separately-verified change.
3. **All-or-nothing per flush.** Either the WHOLE flush renders on the GPU or the WHOLE flush
   runs the proven CPU replay — never mix backends inside one flush; that keeps framebuffer/Z
   coherence trivial. Count fallbacks; a nonzero rate on a new route is the signal to widen
   support, not a bug report.
4. **Read the result BACK into guest VRAM each flush** (one `glReadPixels`, swizzled with the
   SAME address functions the CPU `writePixel` uses). Present latch, frame dumps, golden gates,
   and any guest VRAM read then work UNCHANGED — the whole existing verification harness stays
   valid. Untouched pixels round-trip losslessly (RGBA8 upload→readback), so the window only
   needs the batch's scissor union.
5. **Texture residency needs CONTENT-HASH revalidation on top of write-generation
   invalidation.** PS2 games re-upload texture bytes EVERY frame ("transfer is cheaper than GS
   space"), so per-page write-gens alone re-decode everything each frame (measured: hits ≈ 0).
   Hashing the texture's source pages (~64-256KB, far cheaper than a deswizzle+CLUT decode)
   detects byte-identical re-uploads and keeps the GPU copy resident. Keep the page-gens too —
   they are what makes RTT-as-texture and CPU-written pages visible to the cache.
6. **GL threading:** create the persistent GPU thread's shared context with the
   release-before-share dance (release the main context, `wglCreateContext` + `wglShareLists` on
   the new thread, restore main) — `wglShareLists` fails `ERROR_BUSY` while either context is
   current anywhere. Keep blocking submission as the correctness baseline until exclusive submit
   time proves it can pay. Removing a `future.get()` is not an async design: the submitted batch
   must own immutable storage until completion, and residency/depth/publication commit, rollback,
   and presentation must remain ordered behind a completion fence. A reused/static front-end batch
   or immediate post-submit commit makes a queue-only wrapper unsafe.
7. **Soak detectors need a control arm.** A median-based chroma/brightness grid scan over a
   dense per-tick dump flags the game's OWN fade/lighting animation too — run the identical scan
   on a same-length default-path control and compare PROFILES: isolated few-tick bursts in
   stable cells = a race; smooth broad deviation present in both arms = the scene. Window out
   boot/fade-in before taking medians.
8. **Skipping the per-flush readback (GPU residency) pays ONLY when the CONSUMERS are on the
   GPU too — count the consumer edges FIRST.** A DC2 instance built a flawless skip-readback
   residency model for its render-to-texture targets (row-window dirty tracking, generation
   invariant, materialize-on-CPU-consumer edges — zero invariant failures) and won only ~4.5%,
   because ~90% of the family's draws still fell to the CPU replay (state-classifier rejects,
   NOT the suspected depth issue — instrument the reject REASON, the obvious theory was wrong)
   and the guest re-uploaded scratch content into the same rows every frame. The round-trip
   just relocates to the consumer edges. Order of operations: (a) per-edge counters (who reads
   the target: later GPU draws, CPU fallbacks, transfers, uploads-into-the-pages) BEFORE any
   residency mechanism; (b) widen GPU coverage of the consumers first; (c) route the guest's
   per-frame uploads INTO the resident surface, don't materialize around them. Also: scope
   residency footprints and generation invariants to the actually-rendered ROW WINDOW — GS
   layouts pack neighbor targets and streaming-texture pages a few pages away, and a
   whole-target range false-triggers on them every frame.

## §3 Native Renderer Admission, Aliasing, and Presentation

Once a GPU path is correct for supported batches, the next large wins often come from eliminating
whole-batch CPU fallback. Use this protocol:

1. **Profile success and fallback separately.** Split successful GPU work into prepare / submit /
   publication, then census fallback reasons and shapes. A cheap successful batch does not imply
   GPU overhead is the bottleneck; a conservative reject may be replaying the whole batch on CPU.
2. **Relax alias guards only with exact physical evidence.** Conservative range/page functions
   often add slack for unaligned bases or unknown swizzles. Never delete that slack globally.
   For a measured aligned tuple, enumerate the exact pages/blocks touched by both rectangles using
   the real PSM geometry; admit only if the sets are disjoint. Unknown, unaligned, wrapping, or new
   tuples fail closed through the old guard.
3. **Residency is a temporal ownership mechanism, not a readback toggle.** Track which version is
   newest (FBO or guest VRAM), every CPU/draw/transfer/texture consumer, and the first boundary that
   must materialize it. Generation equality alone is insufficient if a later writer republishes an
   older version.
4. **An internal oracle does not prove final composition.** Batch-local CPU/GPU equality can pass
   while a downstream consumer sees the wrong temporal version. Choose exactly one graphics route:
   the normal composed-output route most likely to expose the changed temporal boundary. Inspect
   its full-frame distribution across that route's transition.
5. **Make the presentation gate reference-backed.** When hardware/PCSX2 reference images exist,
   record the exact reference path, route state marker, capture clock/tick, and a landmark checklist
   covering the whole image — including left/right edges and background geometry, not only the
   central subject. Capture the candidate, its kill/control, and the rebuilt final default at that
   same point. Aggregate pixel counts, chroma grids, and local oracles are supporting evidence; they
   can miss a large spatial composition defect.
6. **Test fresh first-use state separately from warmed state.** Start a new process and verify the
   first eligible GPU batch, then run the long oracle/soak. A special backend branch must submit
   every texture, sampler-completeness, combine, wrap, and mode state it reads; it must not inherit
   state from a prior draw. A batch-1-only failure is still a promotion blocker.
7. **Bisect a presentation regression through the control hierarchy.** Hold route/tick/reference
   fixed and test: candidate kill → native-stack master kill → architecture-family kill → individual
   promoted-slice kills. This distinguishes a new regression from an older default-on defect exposed
   by the current review. Either way, a defect in the shipping default blocks promotion. Re-test the
   rebuilt default with no diagnostic kill flags after retiring the culprit.
8. **Bound repairs by architecture.** Repair a missing state submission, edge, or narrow proof
   in-phase when evidence identifies it. If correctness needs a new ownership/versioning mechanism,
   revert the unsafe behavior or keep it explicit opt-in, preserve only diagnostics needed for the
   named blocker, and open a focused follow-up phase.
9. **Separate mechanism proof from payoff proof.** Fewer flushes, readbacks, fallbacks, or uploads
   prove that the intended edge moved; they do not prove the frame became faster. Require both the
   mechanism counter and a separated end-to-end frame-time result.
10. **Use same-executable, reverse-order A/B.** Prefer cached default-off/kill switches in one binary,
    use the same route and warm-up, collect at least three steady windows per arm, then reverse arm
    order. Report ranges as well as means/medians. Overlapping arms or a sign change under reversed
    order is inconclusive, not a win. Any downstream presentation regression blocks promotion even
    if pooled timing improves.
11. **Retire failed behavior safely.** Default-disable or revert correctness-invalid behavior.
    Preserve an explicit opt-in only when it is required to reproduce or measure a named follow-up;
    otherwise remove its flags, helpers, and behavior-only counters. Keep small cached diagnostics,
    document the exact blocker and next mechanism, and prevent accidental re-promotion.
12. **Validate a derived/incremental surface against a freshly-recomputed authoritative reference,
    not against the path you are replacing.** When an incremental update maintains a surface a
    batched recompute would otherwise produce, the exact oracle is: build the incremental result,
    force a full recompute of the SAME inputs into a scratch target, read both back, compare every
    pixel. Keep it self-contained (own scratch resource, own gate) so it neither perturbs the
    shipping publication/materialize path nor depends on its side effects. If a prior phase already
    proved the full recompute equal to the real downstream, this transitively certifies the
    incremental one. `bad==0` over hundreds of millions of pixels is the promotion floor — it proves
    two internal surfaces are equal, NOT that either is presented correctly, so it never replaces
    normal-composition review.
13. **A GS swizzle address function usually factors into ONE page-sized table — that is a free exact
    win on every bulk pack/unpack loop.** Check the function's algebra rather than micro-optimising it:
    if its only dependence on the high bits of x/y is a linear page step, then
    `addr(x,y) = rowBase(y) + pageStep(x) + tbl[y & (pageH-1)][x & (pageW-1)]`, and you can build `tbl`
    by calling the original function over one page — so the table IS the function, not an
    approximation. Then look for horizontal adjacency in the column table: if two neighbouring pixels
    from an even x land one word apart, an even-width row packs as double-width stores. Finally,
    distinct `(x,y)` mapping to distinct words means disjoint ROWS own disjoint memory, so row lanes
    are exact by construction. (Real case: 0.463 → 0.119 ms/frame, bit-exact over 218 M pixels; the
    table+pairing alone was 80% of it, lanes the rest.) Mirror-image loops often diverge — one
    direction may already be parallelised while the other is still scalar; audit both.
14. **Scalar-lane float loops vectorize BIT-EXACTLY under the right compiler contract — check the
    contract, then it is a free exact win.** On MSVC `/O2` with no `/arch:AVX2`-class FMA and
    default `/fp:precise`, SSE2 is identical to the scalar lane loop it replaces, so no tolerance
    argument is needed: `addps/subps/mulps` are per-lane IEEE-754 single with the same rounding,
    and no FMA contraction is possible (a multiply-add stays two roundings). Operand order is the
    subtlety — `_mm_max_ps(a,b)` is `a > b ? a : b` and `_mm_min_ps(a,b)` is `a < b ? a : b`, so
    they match a legacy ternary only if written in the same order (this also preserves its NaN
    and signed-zero behaviour); `_mm_cvttps_epi32` matches what MSVC x64 emits for
    `(int32_t)(float)` (`cvttss2si`), including the 0x80000000 out-of-range result; and
    `_mm_cvtepi32_ps` is exact for every int32. A **masked bit-select store**
    (`or(and(mask,new), andnot(mask,old))`) is value-identical to a chain of conditional per-lane
    stores, because masked-off lanes are rewritten with their own bits. **Verify anyway**: run the
    fast path, restore state, re-run the legacy path from the same pre-state, and compare every
    field the operation can write, aborting on the first mismatch. Record the toolchain contract
    in the phase's Stale-When — changing the FP model or arch flags silently invalidates it.
15. **Before building a scheduled mechanism, grep for it — a prior phase may have left it built and
    disabled as substrate.** Residency/alias/wave/delta machinery is often committed default-off with
    a kill or an `Initialized`/`On` gate as a stepping stone. Enabling and *measuring* existing
    substrate (then closing it exactly with the premise gate) is far cheaper than re-deriving it, and
    is what the phase actually asks for. Read the gate that disables it: it usually encodes the exact
    reason the prior phase judged it not-yet-payable.

---

---

## §4 GPU RESIDENCY — what a "slot" actually costs, and the four ways it wins nothing

Hard-won from an arc that added five residency slots: two were transformative, three were neutral or
negative, and the difference was never the slot.

### 4.1 ⭐⭐⭐ A residency slot is INERT until its CONSUMER contract is widened in the same edit
The clearest instance: a new slot was given to a route's hottest offscreen target, and the frame time
did not move. The mechanism census explained it in one line — **1,698 GPU waves, 1,667
materializations: 98% of the resident content died immediately.** The target was going resident and
then being pulled straight back to guest VRAM, because the **display atlas compositor's source
contract still said "slots 0..4"** and refused the new index, forcing a near-immediate readback.

Extending that compositor to the new slot is what converted the same slot into the win.

> **Before adding a residency slot, enumerate every CONSUMER that reads resident content — the
> compositor/atlas, the texture sampler, the depth sync, the publication path — and check each one's
> own index bound. A slot the consumers reject is a slower path than no slot at all**, because it
> pays the upload *and* the materialize.

**The tell:** `waves ≈ materializations`. Print that ratio for any new target; anything near 1:1
means the content never survives to be used.

### 4.2 Residency is SCENE-SPECIFIC — a slot that deletes 55 ms on one route is neutral on the next
The same slot machinery measured, on the same binary:

| target | route | result |
|---|---|---|
| `0x159` | `s05` cutscene | **~65 → ~40 ms/f** (then `0x13a` took it to 17.7–18.4) |
| `0x159` | Georama | 30.1–31.2 vs 30.4 baseline — **neutral** |
| `0x159` | Dungeon 6 | 27.9–28.5 vs 27.6 — **slightly worse** |
| `0x181` | Georama (its own hottest RTT) | **neutral/worse**, 31–32 vs 30.4 |

A target is hot in the scenes that draw to it and absent elsewhere, so **quote every residency
result per route and never generalise one.** ⛔ Do not blanket-enable slots: measure each, on the
route that motivated it *and* on at least one other.

### 4.3 ⛔⛔ Widening residency can be a CORRECTNESS change, not just a speed one
Admitting more targets means more pages the GPU owns while the guest CPU is still writing. When the
two collide the port logs an ownership-invariant escape and materializes to recover — and that
recovery is a *repair*, not a design. One such widening measured a clean perf win on its motivating
route and shipped a **user-visible graphical defect on every player jump/dash**, because the chosen
gate contained no player-controlled motion. See `17 §1 Laws 0/0a`. **Gate residency on a motion route,
and require the ownership-invariant counter to read ZERO.**

### 4.4 An alias family is admitted WHOLE or not at all
Where several targets sit a few pages apart in VRAM they alias. Admitting a subset splits ownership
across the family and makes **both** sides escape: dropping the single worst offender from a
six-target admission measured **+11.4%** where admitting all six measured **+3.4%** — the partial
configuration was three times worse than the thing it was trying to improve. Per-target masks are
bisect **diagnostics**; they are not shippable configurations.

### 4.5 Cost model for GPU submissions (order of magnitude, one port, one driver)
A fit across routes put backend submission at **~0.38 ms preparation + ~0.09 ms submit per
submission**, at 40–60 submissions/frame — i.e. **~19–28 ms/frame of pure submission overhead**
before any drawing. That makes "fewer, larger submissions" a real lever, but note what refuted its
naive form: merging same-texture runs cut logical GL draw count ~2× and moved the heavy window
**not at all** (43.5–45.1 ms both arms). Batch *count* is not the cost; the per-submission fixed
setup is, and only a change that reduces **submissions** (not draws within them) touches it.

### 4.6 Exactness traps specific to a GPU VU/raster port
- **The GPU flushes denormals; the CPU does not.** A bit-exact comparison found the host CPU
  treating a raw `+0x19` as a real subnormal while the NVIDIA GLSL path flushed it to zero *before*
  the comparison, changing which operand a `MAX`/`MIN` selects. Dead/masked-off lanes are where this
  surfaces first, because nothing else observes them. Repair the *selection*, not the bit pattern,
  and scope it to the exact measured signature rather than normalising denormals globally — a broad
  normalisation immediately broke a different sequence that the GPU had been preserving correctly.
- **A large private array in a compute kernel is a runaway, not a slow path.** Holding 32 KiB of
  code/RAM in one invocation's private arrays produced **99% GPU load with the window unresponsive
  and the CPU idle**. Keep working state in a shared/workspace image, not per-invocation privates.
- **An "exact" GPU executor can still be refuted on throughput alone:** exact 16-kick batches
  measured **32–340 ms**, with *compute* dominating — not the 1 MiB transfer. Price the compute
  before designing the transport.
- **A backend predicate written for the textured case will reject the untextured one.** A solid
  (TME=0) sprite was rejected by a path that assumed every draw to that target was textured-exact;
  the fix was to gate the exact sampler on `texKey != 0` and let solids take the normal shader.
