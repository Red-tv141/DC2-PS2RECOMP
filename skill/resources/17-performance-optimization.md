# Reference: Performance Optimization — Correct First, Fast Second

> **Load this when the game runs CORRECTLY but too slowly** — low FPS, stutter, a headless test
> window that only reaches a few hundred frames in 30 s, input scripts that never fire because the
> target frame is never reached. Do NOT load this while a correctness bug is open: optimizing a
> wrong pipeline wastes the work and destroys your A/B baselines.

---

## §1 Doctrine — The Four Laws

1. **Correctness before speed.** Never optimize a subsystem with an open correctness bug in it.
   An optimization changes timing and code shape — it will smear the evidence you need for the bug.
2. **Measure before touching.** No optimization without a profile or counter proving WHERE the time
   goes. "The rasterizer is probably slow" is a hypothesis, not a diagnosis. The hot spot in a
   recompiled port is frequently NOT where intuition says (it's often logging, dispatch, or a mutex —
   not the math).
3. **Behavior-identical, verified.** After every optimization, re-run the golden baseline
   (e.g. the title-screen pixel metric, a `.gs` capture diff, the phase regression checks). An
   optimization that changes ANY output is a correctness change in disguise — revert or gate it.
4. **Re-profile the current executable.** Every promoted phase changes the cost graph. Treat an
   older phase's "next hotspot" as a stale hypothesis until the current Release binary reproduces
   it. Measure event frequency and exclusive cost, then compute the maximum plausible payoff before
   designing an architectural mechanism.

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
- **Never NAME a residual.** Deriving a sub-term by subtraction (`bucket = parent − childA − childB`)
  and then giving it a semantic name ("serial parse/register-replay") silently assigns it everything
  the child scopes did NOT cover — including flush cascades, backend waits, and deswizzle nested
  inside OTHER children. A residual is only an upper bound on "unmeasured"; before selecting it as
  an optimization target, bracket the alleged body DIRECTLY with its own scope and reconcile
  against the parent. (DC2 G333→G335: a "~40 ms serial parse/replay/build" residual drove an
  arc-level decision; direct scopes later measured the real parse+build at ~13 ms — the other
  ~35 ms was the upload-edge flush cascade nested inside an `image` child bucket.)
- **Charge deferred work to its producer, not the call that happens to pay it.** The first later
  inline draw/line/upload can inherit the cost of earlier queued primitives. Pair timing with a
  steady-window coverage counter for the alleged payload. If the payload count is zero after the
  scene transition, its callsite timing is attribution noise, not an optimization target.
- **Instrumentation inflates the absolute frame; use it for RELATIVE attribution only.** A run with
  the full profiler/stat gates on can be ~2× the lean frame (measured: ~200 ms instrumented vs
  ~90 ms lean on the same route), while a single event's own timer is unchanged (a ~230 µs composite
  reads ~230 µs in both). Never compare an instrumented-run absolute against a lean-run target or a
  promotion gate, and never quote instrumented frame ms as "the frame." Take the payoff A/B on the
  leanest config that still distinguishes the arms, and use the heavy profiler only to attribute
  shares. (A prior arc repeatedly re-derived pole absolutes from instrumented runs and had to add a
  standing "profiler adds ~24 ms/f, relative attribution only" caveat.)
- **A profiler-run bucket can be MOSTLY the profiler — hypothesis-test the instrument itself
  before chasing the bucket.** Even a "cheap" per-event counter block (one relaxed atomic
  fetch_add + a static tick, under a correctly cached master perf env) measured **~855 ns/event**
  on a worker hot path; at ~15k events/frame that fabricated a ~12.6 ms/f "pole sub-term" which
  survived TWO rounds of narrowing (an inclusive per-event timer split, then a dead-code
  skip-branch that came back neutral) before a segment census re-run **with the master perf env
  OFF** collapsed it to 25 ns/event (real path ~119 ns/event). The decisive, cheap move: re-run
  the SAME fine-grained census with the coarse profiler env removed — if the bucket collapses,
  it was self-cost. Corollaries: (a) attribute buckets only from instruments whose own per-event
  cost you have bounded — per-flush/per-window scopes are fine, per-draw/per-call scopes are
  suspect; (b) never design an architecture slice off a bucket that only exists under the
  profiler env; (c) arm-vs-arm A/B under a constant instrument set remains valid — both arms pay
  the self-cost. (DC2 G312: the "31 ms/f register-replay/draw-capture pole term" did not exist
  lean; the real remaining term was a serial upload-deswizzle loop a third its size.)
- **A census lever can DISABLE the mechanism it censuses — grep the lever's `On()` gates for
  cross-disables before premise-gating anything measured under it.** Some "behavior-pure" census
  flags keep themselves pure by turning OFF an optimization while armed (e.g. a coalescing lever
  whose gate includes `!censusOn()`). A cost measured under such a census describes a world
  WITHOUT the optimization: the bucket can be several times its lean size even though the census
  adds no timing self-cost. This is sneakier than profiler self-cost — the number is a real cost,
  just of the wrong binary. Decisive check: re-measure the target bucket with the minimal,
  behavior-pure instrument set only. (DC2 G337: a "~16 ms/f Z round-trip" premise was measured
  under a census env whose gate silently disabled the Z-readback coalescer; the lean class was
  ~2–3 ms and the built mechanism landed neutral.)

- **When a candidate is worth less than the run-to-run noise, change the EXPERIMENT, not the target.**
  Separate-process A/B carries a per-process offset (thermal state, allocator/page layout, driver
  warm-up) that on a real title was ±1.5 ms on a ~67 ms frame. Five alternating-order pairs then
  reported *opposite signs for two arms of the same mechanism* — which is how a genuinely exact win
  gets retired as "inseparable". The fix is a **within-process A/B**: find a work item that happens
  exactly ONCE per frame, and use the interval between consecutive occurrences as a frame period
  sampled at a fixed phase. Switch the arm inside one process, bucket each period by the arm that
  produced it, and hundreds of interleaved samples per run collapse the process-level term. Three
  nuisances must be removed before the number means anything:
  1. **Warm-up bias** — whichever arm owns the first block inherits the route's load-in. Skip a
     warm-up prefix (~120 frames) before accumulating.
  2. **Parity aliasing** — fixed "N frames A, N frames B" alternation aliases with any workload period
     that is a multiple of 2N. Measured: at N=60 the arm looked 0.79 ms *slower*, and swapping which
     parity got which arm flipped the sign. Always ship an **invert control**, or better, assign the
     arm **per frame from a fixed LCG** — randomisation cannot alias and makes the SEs meaningful.
  3. **Drift inside the run** — both arm means climb together, so a pooled SE overstates the error.
     Also report a **blocked estimator**: difference consecutive periodic prints, take per-window
     deltas, average those. (Real case: pooled se 0.120 ms vs blocked se 0.077 ms, t = −3.2.)
- **A refactor at the shared dispatch site is a change to the CONTROL ARM — and the A/B cannot
  see it.** When you add a fast path beside an existing one, both arms still flow through the
  site you edited, so any wrapper you introduce there is paid by *both* and silently inflates the
  measured win. Real case: wrapping an interpreter's `execUpper` call in a `[&]` lambda that did
  not inline gave the lever-OFF path a lambda frame *plus* the original call (2 calls where the
  pre-phase binary had 1); the control went 67 → 74 ms and the A/B read **−9.8%** for a lever
  worth **−8.4%**. Two siblings of the same mistake in one phase: an inlined per-event census
  histogram cost ~2 ms/frame on the default path (fix: `noinline` hook, so the loop carries only
  a cached-bool test and a never-taken call), and a *second* inlined copy of the fast-path body
  at a site worth 0.69% of executions cost more in hot-loop instruction footprint than that share
  could repay (fix: leave rare sites on the legacy path — more inlining is not better).
  **Protocol:** after editing any shared hot path, re-measure the **control arm alone** against
  the pre-phase absolute before trusting the delta; if the control moved, fix that first. Prefer
  a local `do{…}while(0)` macro over a lambda/helper at hot dispatch sites so the legacy call
  shape stays byte-identical, and delete census counters from the loop once their values are
  recorded. (DC2 G421.)
- **An interpreter's call boundary has a price you may have already paid for — look it up before
  designing.** If an earlier phase won by *skipping* a dispatch for some share of events, divide
  its gain by the events it skipped to get ns-per-call, then multiply by the events that still
  pay it. Real case: a prior phase bought 8.9% of a worker thread by skipping the call for the
  49.5% no-op share ⇒ ~8.6 ns/call ⇒ inlining the *remaining* 0.82 M calls/frame had a ~7 ms
  (~10.5%) ceiling, known before a line was written, and it landed at −8.4%. This is the payoff
  ceiling of §2's premise gate computed from an already-measured constant instead of a new probe.
- **Measure the site's critical-path sensitivity with BALLAST before believing any payoff.** Removed
  work is not payoff (a candidate can sit behind slack, or its thread may not be the pole). Cheap
  direct test: run BOTH buckets on the unoptimized path and have one bucket repeat it N extra times —
  choose a repetition that is **idempotent** (rewriting identical bytes) so output cannot change. The
  measured delta divided by the known added cost is the fraction of work at that site that reaches the
  frame. Multiply an isolated saving by that ratio to get an honest prediction, and compare the
  prediction to the end-to-end result: if they match, the work did not reappear elsewhere. (Real case:
  +1.852 ms of ballast moved the frame +1.717 ms ⇒ 0.93 sensitivity; a 0.344 ms saving then predicted
  −0.32 ms and measured −0.31 ms.) A low ratio retires the candidate for the cost of one run.

### Optimization Premise Gate

Pass this gate before implementing a performance phase:

0. **Attribute every bucket to its THREAD before ranking anything.** In a 2+-thread pipeline the
   frame is max(threads), not sum(buckets): a table of real, correctly-measured costs still
   mis-ranks the frame if all its rows live on a non-critical thread. Get each thread's
   CPU-time/frame (`QueryThreadCycleTime`-class) plus its wall share; only buckets on the max()
   thread — or the max() thread's *wait* slice — are frame-time levers. High onCPU% on the
   critical thread also excludes condvar-based backpressure as an explanation for its hot
   buckets. (DC2 G293/G294 spent two phases on worker-side readback costs whose true frame
   ceiling was the EE thread's 3–8 ms wait slice; the actual pole — the VU1 interpreter at
   72–74% of the frame — was absent from the table because no instrument attributed the
   critical thread's own compute.) **This decays — re-run it EVERY phase, not once per arc.**
   A later arc on the same title spent *seven consecutive phases* grinding 0.1–0.7 ms slices out
   of the GS front end (one of which promoted nothing at all) because the thread table was
   inherited from an older profile. One 60-second re-run showed the *other* worker sitting at
   ~98% of the frame (65.2 ms busy vs a 66.2 ms frame), and the first lever aimed there was worth
   −8.4%. Corollary: re-run it again **after** a big win — the same phase moved the poles to
   ~62 vs ~59 ms, close enough that the next slice on the old pole may buy nothing. (DC2
   G414–G421.)
   - **0a. Audit what each thread counter actually COVERS before ranking with it.** A worker
     "busy" counter that brackets only one branch of the worker loop silently under-reports that
     thread. In a later phase the GS worker's headline field covered only the packet-window branch
     and excluded the frame-boundary closure and apply branches — it read 44–46 ms while the
     all-branch census read **50–53 ms**, which inverted the pole ranking against a VU1 worker at
     45–49 ms. Read the accumulator's definition (and its own comment) before you trust it, and
     prefer the census that explicitly sums *every* branch. (DC2 G424.)
   - **0b. Subtract each thread's WAIT-ON-PEER slice before calling its busy time "work."** After a
     win on one thread its counter can stay flat while its *content* changes from compute to idle
     hand-off waiting. Same phase: the GS worker's total held at ~50 ms but ~8 ms of it became
     "waiting for VU1", so its real work was ~42 ms and the pole had genuinely moved. Ranking on
     the un-decomposed total would have aimed the next phase at the wrong thread. (DC2 G424.)
1. Profile the **current final executable** with the intended defaults, route, warm-up, and steady
   window. Do not reuse a pre-fix profile as the phase premise.
2. Split inclusive buckets until the candidate's exclusive cost is known. Record waits, drains,
   readbacks, and publication separately.
3. Compute a payoff ceiling: `events/frame × exclusive cost/event`. If even deleting the candidate
   cannot meet the phase's payoff target, stop and re-rank the architecture.
   - **A high event COUNT is not a cost until you multiply by per-event exclusive TIME.** A profile
     line or internal counter that says "N per frame" (drains, composites, flushes, uploads) is a
     frequency, not a bucket. Measure one event's synchronous time first. A real instance: a logical
     "576 full recomposites / 1,024 waves" sounded like the dominant inefficiency, but each composite
     was ~230 µs — well under 1.5 % of the frame — so its entire deletion ceiling was below target.
     The count was scary; the time was nothing.
   - **A delta/incremental replacement of a batched op must be censused for changed-fraction AND
     round-trip count first.** Before building "refresh only what changed," histogram how many
     sub-units actually change per refresh. If the source regenerates wholesale every frame there is
     *no temporal coherence to exploit* (measured: ~107 of 128 pages changed every refresh, zero
     no-ops), and the incremental path does the same work while fragmenting one batched
     backend job into many synchronous `future.get()` round-trips — it is exact but *slower* (306 µs
     vs a 230 µs single composite pass). When a whole surface rebuilds each frame, one batched pass
     beats N per-unit updates. Incremental only pays with real coherence or a truly async transport.
4. Count downstream consumers before building residency, async, or deferred-publication machinery.
   A mechanism that moves work to another edge is not a win.
5. Record the rejected premise in the phase log, including the build/defaults that invalidate the
   old measurement. Diagnostics-only closure is a valid phase result.
6. **Power the A/B before you believe a null.** One run per arm, judged from the last few windows a
   harness prints, cannot resolve a lever smaller than the run-to-run offset noise (±1.5 ms in a
   real case). A phase measured its lever "flat" (51.54 vs 51.60 ms) on that basis and nearly
   discarded it; pooling **every** steady window across **4 runs per arm in both arm orders** gave
   −2.45 ms with *no overlap* between the arms' per-run means. Report per-run means so overlap is
   visible, alternate arm order to defeat drift, and treat a null as a result only when the pooled
   confidence interval actually excludes the effect you care about. (DC2 G424.)

### Before designing an algorithm, check the CALL BOUNDARY

A per-element loop that calls a small helper defined in **another translation unit** pays a real
call — and blocks loop-invariant hoisting in the caller — unless the build enables LTO/LTCG. Verify
this from the build flags, not from intuition: a Release preset of `/O2 /Ob2 /DNDEBUG` has **no
`/GL`**, so nothing is inlined across TUs. A real case: a swizzled VRAM upload made ~930k cross-TU
calls per frame; moving the *loop* into the TU that owned the lookup tables — issuing the identical
calls with identical arguments in identical order — gave **2.97×** on the hot format with zero
semantic change, and it was exact by construction because no addressing math was rewritten.

When a fast path re-issues the same leaf calls, the only genuinely new logic is the loop/run
**decomposition** — so point the oracle at that: replay both arms from an identical pre-state and
compare the whole destination buffer plus any resumable cursor, aborting on the first mismatch.
(DC2 G424: 4 MiB of GS memory + the transfer cursor, 40,000+ comparisons, zero mismatches.)

---

## §3 Known Hotspot Classes in a Recompiled PS2 Runtime

Check these IN ORDER — the cheap wins come first. Confirm each with the profiler before acting.

| # | Hotspot | Symptom / check | Fix direction |
|---|---------|-----------------|---------------|
| 1 | **Leftover diagnostic logging** | `printf`/`fprintf`/`std::cout` in a per-frame, per-draw, per-call path; console I/O shows in profile | Delete or env-gate. Format+flush per call is brutally slow. An uncapped *counter* is fine; a per-hit *printf* is not. **Also: DISABLED probes are not free at scale** — dozens of cached-bool-gated probe blocks accumulated over many debug phases in one hot loop still pay their pc/state compares and branches every iteration. Wrap the whole pile behind ONE master "any diagnostic active this run" flag (probe envs OR'd once at entry) with the legacy interleaved body kept byte-identical as the kill-switch arm; require new probes to register in the master list. DC2 G295: gating ~20 dormant probe regions in the VU1 interpreter loop (+ skipping a redundant save/restore) cut the interpreter ~28% and the frame −12%. |
| 2 | **Per-call `getenv()` in env-gated diagnostics** | An env-gated probe (`if (envFlagEnabled("X_TRACE")) …`) sits in a per-vertex/per-draw/per-tag path WITHOUT a `static const bool` cache. Does NOT show as I/O — the µs-class `getenv` (env lock + linear scan on Windows CRT) hides inside the caller's inclusive time, and worker/replay threads SERIALIZE on the CRT env lock. Check: grep hot files for `getenv`/`envFlagEnabled` calls not feeding a `static const` initializer; census per-callsite ns (a cheap handler at 25 ns vs a sibling at 5,000 ns whose only extra feature is an uncached env check = the tell). | Read once into `static const bool` (magic statics are thread-safe); keep an opt-in lever that restores per-call reads as the same-binary A/B control. Real-world cost: ONE uncached line in a per-vertex kick path cost **41% of the whole frame** and masqueraded for four phases as an architectural "parse/dispatch" bucket (DC2 G268) — profile-bucket names lie; census per-descriptor/per-callsite before designing an architectural fix for a bucket. |
| 3 | **Debug/unoptimized build** | You're not on `Release`; iterators/asserts in profile | Verify `CMAKE_BUILD_TYPE=Release` (Ninja: baked at configure; VS generator: `--config Release`). Never "fix" perf while accidentally profiling Debug. |
| 4 | **Guest memory access macros** | `READ32`/`WRITE32`/`READ128` etc. dominate samples — every guest access masks + bounds-checks + MMIO-routes | Fast-path the common case (plain RDRAM range) before the MMIO check; keep the MMIO route for `0x10000000+`/`0x12000000` only. Behavior-identical by construction — still A/B it. |
| 5 | **Function-pointer dispatch lookup** | The indirect-call resolver (address → handler map) hot in profile | Cache lookups; use a flat table indexed by (addr − code_base)/4 rather than a hash map, if the runtime doesn't already. |
| 6 | **VU1 interpreter inner loop** | `ps2_vu1.cpp` dominates; heavy per-instruction decode | Decode-once/cache per microprogram; keep flag/Q-latency semantics EXACTLY (the correctness rows in `15-vu1-gs-debugging.md` §2 are non-negotiable — re-run distinct-lane tests after). **Then inline the operation itself.** Census the dynamic mix first (pairs/run × runs/frame ⇒ ns/pair; a predecoded step much over ~15-30 host cycles means the *step* is the target, not the guest workload). If the hot slots leave the loop through a non-inlined `execUpper`/`execLower` with a wide jump table, execute the dominant families **inline in registers** from a descriptor table indexed by the existing predecode, and fall back for cross-lane/flag-only ops — a families-based table hits ~98% of executions with no cliff. See §3.3 for the bit-exactness rules that make the SIMD form provably identical. (DC2 G421: −8.4% frame, 600 M shadow-verified ops, `bad=0`.) |
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
   frame dumps/window output across transitions and multiple routes.
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

## §4 What NOT to Do

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

For every rejected optimization premise, record the current-binary profile, payoff ceiling,
mechanism counters, reverse-order A/B distribution, normal-presentation evidence, prototype
disposition, and focused next mechanism in the phase fix log. Never summarize an internal counter
reduction as a performance win.

Cross-refs: verification ladder `10-agent-guardrails.md` §4; lever/kill-switch doctrine
`15-vu1-gs-debugging.md` §5; lock model `16-runtime-concurrency-threading.md`; SIMD note
`04-runtime-syscalls-stubs.md` §6; build gate `SKILL.md` §4.
