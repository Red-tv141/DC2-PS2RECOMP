# Reference: Performance Optimization — Correct First, Fast Second

> **Load this when the game runs CORRECTLY but too slowly** — low FPS, stutter, a headless test
> window that only reaches a few hundred frames in 30 s, input scripts that never fire because the
> target frame is never reached. Do NOT load this while a correctness bug is open: optimizing a
> wrong pipeline wastes the work and destroys your A/B baselines.

---

## §1 Doctrine — The Three Laws

1. **Correctness before speed.** Never optimize a subsystem with an open correctness bug in it.
   An optimization changes timing and code shape — it will smear the evidence you need for the bug.
2. **Measure before touching.** No optimization without a profile or counter proving WHERE the time
   goes. "The rasterizer is probably slow" is a hypothesis, not a diagnosis. The hot spot in a
   recompiled port is frequently NOT where intuition says (it's often logging, dispatch, or a mutex —
   not the math).
3. **Behavior-identical, verified.** After every optimization, re-run the golden baseline
   (e.g. the title-screen pixel metric, a `.gs` capture diff, the phase regression checks). An
   optimization that changes ANY output is a correctness change in disguise — revert or gate it.

Every performance change follows the same Verification Ladder as a fix (`10-agent-guardrails.md` §4):
write → build → run → **compare output metric AND time metric** → record in `PS2_PROJECT_STATE.md`.

---

## §2 Measure — Cheapest Instrument That Answers the Question

| Question | Instrument |
|----------|-----------|
| "How fast is it overall?" | A frame counter + wall-clock in the host present loop, printed once per N seconds (NOT per frame). Frames-per-second before/after is your primary metric. |
| "Which subsystem eats the frame?" | Cheap accumulating timers (`QueryPerformanceCounter` / `std::chrono::steady_clock`) around the big stages: guest EE slice, VU1 interpreter run, VIF unpack, GS rasterize, present. Print totals every N seconds. |
| "Which function eats the subsystem?" | A sampling profiler on the Release binary: Visual Studio Performance Profiler (CPU Usage), ETW/WPR, or Very Sleepy. Sampling — never instrument 30,000 runner files. |
| "Is it CPU at all?" | Task Manager / `Get-Counter`: one core pegged = single-thread CPU bound (usual case). All cores idle but slow = lock contention or sleeps — see `16-runtime-concurrency-threading.md`. |

**Instrumentation rules** (same context-survival discipline as everywhere else):
- Counters/timers print AGGREGATES on an interval — never per-frame, never per-call `printf`.
- Remove or env-gate every timer when done (`<PREFIX>_PERF=1`), per the lever doctrine
  (`15-vu1-gs-debugging.md` §5).
- **The env gate itself must be CACHED** (`static const bool on = getenv(...)`), never a per-call
  `getenv`: on Windows CRT that is an env-lock + linear scan (µs-class) and worker threads
  serialize on the lock. One uncached gate in a per-vertex path cost 41% of the frame and
  masqueraded as a "parse" bucket for four phases (hotspot class #2 in §3).
- **Treat every timer as inclusive until proven otherwise.** A timer around an upload, draw, or
  register handler may include a pending command-graph drain, worker wait, GPU readback, or
  dependency barrier. Add child timers around the suspected body and each wait/flush before
  optimizing the caller. A real 155 ms "image upload" bucket contained <=10 ms of CT32 writing
  and ~145 ms of pending graph execution.
- **Charge deferred work to its producer, not the call that happens to pay it.** The first later
  inline draw/line/upload can inherit the cost of earlier queued primitives. Pair timing with a
  steady-window coverage counter for the alleged payload. If the payload count is zero after the
  scene transition, its callsite timing is attribution noise, not an optimization target.

---

## §3 Known Hotspot Classes in a Recompiled PS2 Runtime

Check these IN ORDER — the cheap wins come first. Confirm each with the profiler before acting.

| # | Hotspot | Symptom / check | Fix direction |
|---|---------|-----------------|---------------|
| 1 | **Leftover diagnostic logging** | `printf`/`fprintf`/`std::cout` in a per-frame, per-draw, per-call path; console I/O shows in profile | Delete or env-gate. Format+flush per call is brutally slow. An uncapped *counter* is fine; a per-hit *printf* is not. |
| 2 | **Per-call `getenv()` in env-gated diagnostics** | An env-gated probe (`if (envFlagEnabled("X_TRACE")) …`) sits in a per-vertex/per-draw/per-tag path WITHOUT a `static const bool` cache. Does NOT show as I/O — the µs-class `getenv` (env lock + linear scan on Windows CRT) hides inside the caller's inclusive time, and worker/replay threads SERIALIZE on the CRT env lock. Check: grep hot files for `getenv`/`envFlagEnabled` calls not feeding a `static const` initializer; census per-callsite ns (a cheap handler at 25 ns vs a sibling at 5,000 ns whose only extra feature is an uncached env check = the tell). | Read once into `static const bool` (magic statics are thread-safe); keep an opt-in lever that restores per-call reads as the same-binary A/B control. Real-world cost: ONE uncached line in a per-vertex kick path cost **41% of the whole frame** and masqueraded for four phases as an architectural "parse/dispatch" bucket (DC2 G268) — profile-bucket names lie; census per-descriptor/per-callsite before designing an architectural fix for a bucket. |
| 3 | **Debug/unoptimized build** | You're not on `Release`; iterators/asserts in profile | Verify `CMAKE_BUILD_TYPE=Release` (Ninja: baked at configure; VS generator: `--config Release`). Never "fix" perf while accidentally profiling Debug. |
| 4 | **Guest memory access macros** | `READ32`/`WRITE32`/`READ128` etc. dominate samples — every guest access masks + bounds-checks + MMIO-routes | Fast-path the common case (plain RDRAM range) before the MMIO check; keep the MMIO route for `0x10000000+`/`0x12000000` only. Behavior-identical by construction — still A/B it. |
| 5 | **Function-pointer dispatch lookup** | The indirect-call resolver (address → handler map) hot in profile | Cache lookups; use a flat table indexed by (addr − code_base)/4 rather than a hash map, if the runtime doesn't already. |
| 6 | **VU1 interpreter inner loop** | `ps2_vu1.cpp` dominates; heavy per-instruction decode | Decode-once/cache per microprogram; keep flag/Q-latency semantics EXACTLY (the correctness rows in `15-vu1-gs-debugging.md` §2 are non-negotiable — re-run distinct-lane tests after). |
| 7 | **GS software rasterizer** | `ps2_gs_rasterizer.cpp` per-pixel loop dominates (usually the #1 cost) | FIRST hoist per-triangle invariants out of the per-pixel path (sampler setup, CLUT decode → memoize per-triangle, swizzle-address base, alpha/blend decode) + scanline-narrow the bbox scan; THEN parallelize across disjoint pixels — see **§3.1** (the biggest lever). Do NOT change rounding/blend/sample semantics (verify vs `.gs` capture + same-run per-pixel A/B). |
| 8 | **Guest-execution lock contention / sleeps** | Cores idle, FPS low, threads ping-ponging | See `16-runtime-concurrency-threading.md` — wrong wait granularity (e.g. a 200 µs sleep in a hot yield) caps FPS. Tune wait sites, keep the release-on-wait rule intact. |
| 9 | **Scalar loops in math-heavy stubs/overrides** | Your own handwritten override shows hot | Vectorize with SSE intrinsics (`04-runtime-syscalls-stubs.md` §6). Test with DISTINCT per-lane values after (`10-agent-guardrails.md` §2.1) — vectorizing is exactly where lane bugs are born. |
| 10 | **Per-call allocations / copies in handlers** | `malloc`/`memcpy` hot inside a stub called per frame | Preallocate/reuse buffers. Respect allocator-family coherence (§3.6 of `10-agent-guardrails.md`) — never introduce a second allocator path. |

---

## §3.1 Parallelizing the GS Software Rasterizer — the Big Lever + its Correctness Contract

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

## §3.2 Moving the Software Rasterizer to the GPU (LLE) — the Endgame Lever + its Contracts

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
   current anywhere. Submit batches synchronously (blocking future) from whichever thread owns
   the flush; the queue serializes context access.
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

## §3.3 Native Renderer Admission, Aliasing, and Presentation

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
   while a downstream consumer sees the wrong temporal version. Gate promotion on normal composed
   frame dumps/window output across transitions and multiple routes. If character parts, terrain,
   shadows, or overlays disappear, keep the behavior default-off even when timing improves.
5. **Bound repairs by architecture.** Repair a missing edge or narrow proof in-phase when evidence
   identifies it. If correctness needs a new ownership/versioning mechanism, revert the unsafe
   behavior or keep it opt-in, preserve diagnostic counters, name the exact consumer blocker, and
   open a focused follow-up phase.

---

## §4 What NOT to Do

- **No speculative micro-optimizations** in generated `runner/*.cpp` — you can't edit those files
  anyway (Prohibition #2), and the compiler already optimizes them.
- **No "optimization" that skips guest work** (dropping draws, skipping VU programs, frame-skipping)
  as a default. If used as a stopgap, it's a band-aid: env-gate it, default OFF, document in the
  state file with its removal condition.
- **No threading of GUEST EXECUTION for speed** without re-reading `16-runtime-concurrency-threading.md`
  — breaking the single-guest-lock model is a rewrite, not an optimization. **The ONE sanctioned
  exception is parallelizing the GS *rasterizer* across disjoint pixels (§3.1)** — it never touches
  the guest lock and is bit-exact by construction. Keep it default-OFF until soaked across all scenes.
- **No toolchain flag roulette.** `clang-cl + Ninja + Release` is already the sanctioned optimum
  (`03-ps2recomp-pipeline.md` §4). Changing global flags forces a mass rebuild — Build Gate applies.

---

## §5 Record It

For every accepted optimization, one line in `PS2_PROJECT_STATE.md → Learned Patterns`:
`<hotspot> cost <N>% frame time, fixed with <what>, FPS <before> → <after>, baseline metric unchanged`.
If it changed an output metric even slightly: it is NOT an optimization — reclassify as a behavior
change and route it through the normal fix taxonomy.

Cross-refs: verification ladder `10-agent-guardrails.md` §4; lever/kill-switch doctrine
`15-vu1-gs-debugging.md` §5; lock model `16-runtime-concurrency-threading.md`; SIMD note
`04-runtime-syscalls-stubs.md` §6; build gate `SKILL.md` §4.
