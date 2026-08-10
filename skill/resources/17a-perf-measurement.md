# Reference: Performance MEASUREMENT — Rank, Instrument, and Power the A/B

> **Load this the moment a performance phase starts, before any lever is designed.** It answers
> *how do I find out where the time goes, and how do I know a change helped?* Sibling files:
> `17-performance-optimization.md` (doctrine + router), `17b-perf-levers.md` (what to build once
> this file has named the target), `17c-perf-gs-pipeline.md` (GS rasterizer/LLE playbooks).
>
> **The single most expensive mistake in perf work is optimizing a correctly-measured bucket that
> lives on a non-critical thread.** Everything in §A exists to prevent it.

---

## §1 Measure — Cheapest Instrument That Answers the Question

| Question | Instrument |
|----------|-----------|
| "How fast is it overall?" | A frame counter + wall-clock in the host present loop, printed once per N seconds (NOT per frame). Frames-per-second before/after is your primary metric. |
| "Which subsystem eats the frame?" | Cheap accumulating timers (`QueryPerformanceCounter` / `std::chrono::steady_clock`) around the big stages: guest EE slice, VU1 interpreter run, VIF unpack, GS rasterize, present. Print totals every N seconds. |
| "Which function eats the subsystem?" | A sampling profiler on the Release binary: Visual Studio Performance Profiler (CPU Usage), ETW/WPR, or Very Sleepy. Sampling — never instrument 30,000 runner files. |
| "Is it CPU at all?" | Task Manager / `Get-Counter`: one core pegged = single-thread CPU bound (usual case). All cores idle but slow = lock contention or sleeps — see `16-runtime-concurrency-threading.md`. |

**Instrumentation rules** (same context-survival discipline as everywhere else):
- Counters/timers print AGGREGATES on an interval — never per-frame, never per-call `printf`.
- Remove or env-gate every timer when done (`<PREFIX>_PERF=1`), per the lever doctrine
  (`15b-gs-state-and-capture-ab.md` §3).
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
- **A PC sampler row resolving to `+0x0` is a function entry sampling artifact, not a time hotspot.**
  On threads where thread context inspection (`SuspendThread` / `GetThreadContext`) biases toward function entries, sampling profilers count call frequency rather than execution time. Always verify sampler rows against disassembly and direct wall-clock scopes before ranking buckets. (G509 proved `[G446:gsprof]` top row `g415_backend_set_color_window+0x0` was a 9-instruction function entry called 33.4×/f, not 12% of thread wall-clock.)
- **Rank buckets by (SPAN × CONVERSION), where conversion is a property of the THREAD.**
  A `ms/f` span on a worker thread is a price only to the extent it reaches the frame clock. Front thread converts at ≈1.0× (it *is* the frame); GS worker converts at ~0.7× for adding CPU work / ~0.1× for deleting blocked GPU-wait (G524); band-replay worker lanes convert at **≈0.05×** (G530: burning 27.8 ms/f of CPU moves the frame only ~1.1 ms). Never rank buckets across threads on raw span alone.
- **A partial deletion probe bounds full deletion from below ONLY when the deleted parts are INDEPENDENT.**
  If four operations share a cache line, an address chain, or register pipeline latency, deleting three of them (e.g. `POINTONLY` deleting 3 of 4 bilinear taps) leaves the remaining one paying the line fill and setup cost. The partial delta bounds full deletion from below only when the sub-operations are non-overlapping.

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

### Before pricing what a hot loop EXECUTES, print where its data LIVES

`alignof` is not a detail on any struct read or written with unaligned SIMD intrinsics
(`_mm_loadu_ps` / `_mm_storeu_ps`). Those intrinsics make misalignment *correct*, which is exactly
why it is invisible — no crash, no wrong result, just stalls. One interpreter's register file had
its widest member a `float`, so `alignof` was 4 and the struct landed at `base % 64 == 52`: a
quarter of the vector registers and **all** of the accumulator split a 64-byte cache line on every
access, and a line-splitting store never store-to-load forwards, so each dependent step of a
multiply-accumulate chain paid a store-buffer drain (~15–20 cycles) instead of ~5-cycle forwarding.
`alignas(64)` was worth **7% of the frame** after four phases had priced the same loop's instruction
mix, footprint, store width, pointer aliasing and addressing at ~zero and concluded "the residual is
stall." Print `reinterpret_cast<uintptr_t>(&x) & 63` at a hot entry instead of assuming.

**When you sweep the rest of the tree for it, the pass criterion is `& 15 == 0`, not `& 63 == 0`.**
A 16-byte access at a 16-byte-aligned address lies wholly inside one 64-byte line whichever slot it
occupies; only sub-16 alignment can split one. So `alignas(16)` members, and heap blocks from
`operator new` (16-byte `__STDCPP_DEFAULT_NEW_ALIGNMENT__`), are already clean — and a subsystem
that contains no SIMD intrinsics at all cannot carry the defect however hot it is. Grep for the
intrinsics first; that usually turns a whole-tree sweep into a handful of objects.

---

## §2 Laws from a long multi-phase perf arc — measurement half

> Distilled from a 14-phase arc that took one title from ~194 ms/f to ~48 ms/f
> (source: DC2 G418–G431). Ordered by how early in a phase they bite. Every one
> was learned by getting it wrong first. The build-side laws are in
> `17b-perf-levers.md` §2.

These are the rules that a 14-phase, ~194 ms → ~48 ms arc paid for. They are ordered by how early
in a phase they bite. Every one of them was learned by *getting it wrong first*.

### A. Ranking threads and sites

1. **Rank poles with a DERIVATIVE probe, never with an occupancy timer.** `busyMs/f`, an inclusive
   `totalMs/f`, or a stall counter are thread *occupancy*: two saturated threads look co-limiting
   even when one converts 1:1 to frame time and the other converts nothing. Inject a known busy-spin
   into thread A and read the frame; repeat for thread B. Measured in one arc: 0.97 for one worker
   vs 0.02 for the other, while their occupancy counters were within 3 ms of each other and the
   hand-off written from those counters had aimed the next phase at the wrong thread.
2. **Build the injection probe for EVERY candidate thread, and prefer a spin to a deletion.** If you
   can only price thread B by *deleting* one of its edges, you have an incorrect-output probe with
   an unproven workload invariant, not a sensitivity. A symmetric busy-spin inside each worker's own
   busy interval is a few lines and is the sound instrument. Multiply `<us>` by that worker's live
   events/frame to state the injected total.
3. **A pole ranking is binary-specific evidence, never a standing fact.** Two promoted levers later
   the same arc re-measured 0.49 for the thread that had been 0.97, while the other went 0.02 → 1.05
   — the workers had crossed. Re-run the gate at the START of every phase and again AFTER a big win.

3a. **…and it is not even SESSION-specific: it is one HOST STATE, and a session contains more than
   one. Find the cheap LEADING INDICATOR that names the state, and read it before you probe.** One
   arc measured its two workers' sensitivities twice, **25 minutes apart on the same unmodified
   binary**, and the whole ranking inverted: worker A 0.03 → **0.96**, worker B 0.75 → 0.08. Nothing
   in the code changed. The entire difference lived in ONE census bucket — a GPU readback at
   5.9 ms/f over 6 reads versus 2.9 — and a single cheap counter (`backendMs/f`, 8.3 vs 4.6) had
   predicted both readings. Four earlier sessions had noted the correlation and treated it as
   background colour; taking the probes in *both* states turned it into a test.
   Consequences, all of which cost that phase real runs:
   * **Take the light occupancy census FIRST and let the indicator choose the subsystem.** It is one
     run and it decides whether the phase's lever can convert at all.
   * **The CURVE's shape is a state property too.** In the state where the thread led, the two
     magnitudes read 0.96 and 0.96 — *linear*, so the sensitivity is a point estimate. In the other,
     0.03 and 0.52 — strongly superlinear, because the probe was sitting on a THRESHOLD.
   * **A sensitivity SUM built from point estimates is meaningless when any probe is on a
     threshold.** The threshold state summed to 0.70, below the "a whole unit is unprobed" line, and
     would have sent the phase hunting a fifth unit; the marginal slope gives 1.43, the ordinary
     serial-chain signature.
   * **Slack is measurable, not inferable.** `slack = injection@big − frameDelta@big` (here
     3.70 − 1.92 = 1.78 ms/f). Below that, a lever on that thread converts to exactly zero.
4. **Audit what a thread counter actually COVERS.** A "busy" accumulator that brackets only one
   branch of the worker loop silently under-reports that thread (one read 44–46 ms where an
   all-branch census read 50–53, which inverted the ranking). Read the accumulator's definition.
5. **Subtract each thread's WAIT-ON-PEER slice before calling its busy time "work."**
6. **Attribute INSIDE the pole thread, not just to it.** The pole worker also ran packet submission
   and a queue; splitting it showed 97.6% in one loop and 2.4% elsewhere — three different veins
   with three different levers, only one worth sizing.
7. **Size the phase's CLAIM to `pole − runnerUp`, not to the saving.** A lever on the pole converts
   to frame time only until that thread crosses under the second. One lever removed 11–13 ms from
   the pole worker and moved the frame 0.5 ms. Compute the crossover first and state it as the
   ceiling. **But a thread-level win with a flat frame is still worth promoting when it removes a
   FLOOR** — say that explicitly, and never restate the neutral frame number as a win.

7b. **A derivative spin and a deletion probe measure DIFFERENT things — only the deletion probe
   prices a lever.** In one arc a worker thread measured sensitivity **1.15** (injected busy CPU on
   it propagated to the frame better than 1:1), and yet deleting *every named block of work resident
   on it* — the whole primitive-submission path plus the entire GPU backend it fed, ~35 ms/f across
   two threads — moved the frame **2.8 ms**, and deleting a second 7.6 ms/f block on it was not
   measurable at all. What transmits can be the serialized hand-off chain (queue → collect → wake),
   not the work volume inside it. So: rank threads with the spin, but **before building anything,
   delete the candidate work and measure the frame**. Corollary: a worker's inclusive `totalMs/f` can
   be pure OCCUPANCY — one stayed at 46.7 ms/f with 100% of its named work deleted, while a peer
   thread's stall counter absorbed the whole difference (0 → 33 ms/f).

7d. **A verdict of "measured neutral, left default-off" EXPIRES with the pole exactly like a
   deletion probe's — and nobody ever re-reads it, because it is not in the do-not-re-open table,
   it is a flag sitting in the source.** In one arc a phase built a one-line lever, measured it
   neutral across runs *while a different thread bound the frame*, and shipped it behind an opt-in
   env flag. Twenty-seven phases later, with that thread at sensitivity 0.97, the identical code was
   worth **−6.3 ms/f, 15.9% of the frame** — the arc's largest lever in fifteen phases, for one
   `glFlush`. A lever multiplied by a sensitivity of ~0 was never actually measured. **After every
   pole flip, grep the tree for un-promoted opt-in perf flags and re-price them BEFORE designing new
   work.** It is the cheapest phase you will ever run: no design, no build, one paired A/B per flag.

7c. **Run the derivative spin as a PAIRED WITHIN-PROCESS A/B, alternating the arm in BLOCKS of
   frames.** Two separate failures make the obvious designs unusable, and both were caught only by
   running the probe against itself.
   * *Across runs.* The steady frame level is set at process start; in one arc it varied 44.8–48.6
     ms run-to-run while each run's own windows sat within ±0.3 ms. A probe that injects ~5 ms
     cannot be read off a difference of two run means with that spread: two order-balanced blocks
     had **both** probes disagreeing with **themselves in sign** (+1.96 / −0.83 and −2.45 / −0.27),
     with the baseline arm alone moving 4.45 ms between blocks. No affordable run count fixes this;
     change the design, not the sample size.
   * *Within a run, per frame.* If the runtime pipelines frames across threads, work injected on a
     candidate frame partly lands in the NEXT frame period — and under per-frame alternation that
     successor is a control frame half the time, so the candidate's cost leaks into the control
     bucket. The same probe read sensitivity **0.03** at one frame per arm and **0.30** at 16, on
     the same binary in the same session. Hold one arm for a block of frames, drop the single
     boundary frame per block, and report the blocked t statistic.
   Done this way a thread ranking that had inverted four times across an arc became a t ≈ 10–18
   result from a single 75-second run. **Instrument-cost corollary:** a probe consulted per-kick or
   per-window (thousands of times per frame) must not do an atomic read-modify-write per call to
   record which arm ran — that cross-core cost is charged to BOTH arms and inflates the baseline the
   probe divides by. Memoise on a per-frame sequence counter, and never on the arm VALUE: a random
   arm repeats on consecutive frames about half the time, so a value-keyed memo silently stops
   marking the frame and the tick discards it as unattributable.

### B. Trusting an instrument

8. **A profiling census can inflate the very bucket it measures.** Timestamps around each blocking
   backend call read one bucket at 13.0 ms/f where an independent clock in the same run read 7.9.
   That 5 ms error was the difference between "the threads are tied" and "one leads by 4 ms".
   Cross-check any census bucket that wraps a blocking call with a second, independent clock.
9. **An inclusive timer around a FLUSH measures the work it DRAINS, not the flush.** The largest
   worker bucket in one arc (21–23 ms/f over ~10 executes/frame) looked like recoverable edge
   overhead; deleting the edge outright bought **0.36 ms**, because the batches simply executed at
   the frame boundary instead. Before designing a mechanism to break a dependency edge, delete the
   edge in a throwaway arm and read the frame — a one-line probe can retire a multi-phase design.
10. **When a bucket's WALL time is ~10× its CPU time, the lever is the ROUND-TRIP, not the work.**
    A queued backend job measured 0.053 ms wall / 0.005 ms CPU: the front thread was blocking on a
    future, not computing. An exact lever that deleted real work *inside* that job moved wall
    0.053 → 0.049 and the frame not at all. Compare wall to CPU before optimizing anything queued.
10a. **A split that names "drain" and "transfer" may be naming a SCHEDULING DEFECT, not a cost.**
    A `glFinish`-delimited probe reported 0.93 ms of "drain" plus 0.72 ms of "transfer" per GPU
    readback, and six phases read the drain as conserved work — the GPU is behind, nothing to
    recover. It was a host defect: 34 render batches per frame left their commands queued and
    *nothing in the frame ever told the driver to start*, so the one fence paid the whole backlog.
    One `glFlush` per batch took drain to 0.23 **and transfer to 0.31** — proof that the
    `glFinish` split does not cleanly separate backlog from transport either. **Before pricing a
    wait as work, ask what happens if the work is merely KICKED.** Generalises past GL to any
    deferred/batched submission interface (command buffers, async I/O queues, write-behind caches).

10b. **A PC sampler names the wait PRIMITIVE; only per-edge wall counters name the EDGE.** After a
    sampler found a pole thread ~51% blocked in `NtWaitForAlertByThreadId`, the tempting next step is
    a stack walk on the suspended context (dbghelp-vs-heap-lock deadlock risk, forced-low sample
    rate). **Per-edge wall counters are strictly better and almost free:** put one `steady_clock`
    pair on each edge at which the thread can block, and **subtract their sum from the accumulator
    that already measures the span** rather than adding a "CPU" bucket — the residue is then CPU by
    construction and no symbolisation runs at all. Restrict counting to the target thread with a
    `thread_local` flag set at that thread's entry; other threads reach the same entry points and
    must not pool in. Seven such counters reproduced the sampler's two headline numbers from an
    unrelated clock **and** killed two live hypotheses in the same run (0.70 and 0.05 ms/f against a
    16.7 ms/f total).
10c. **Sweep a latency-hiding lever's BUDGET — the shape of the curve identifies the mechanism.** A
    spin-then-block wait read −1.38 / −1.23 / −1.35 ms/f at 120 / 400 / 1500 µs of spin budget: one
    flat line. **A saving that does not scale with its budget is a LATENCY removed, not work** — here
    exactly one thread wake (~18 µs × ~74 handoffs/frame), and the flatness is the instruction to
    stop: no deeper spin, adaptive backoff, or lighter completion primitive exists to build. The
    mirror case has a **threshold**: the same phase's worker-side spin was worth −0.55 at 400 µs but
    −0.20 and insignificant at 150 µs, because its budget must outlast the ~430 µs mean gap between
    item arrivals.

11. **A "bucket" name is not a mechanism.** One arc's `parserOther` was literally
    `packetMs − drawMs − imageMs` and was dominated by a blocking readback handler, not by the
    parsing its name implied. Read the arithmetic that produces a profile line before targeting it.
12. **A high event COUNT is not a cost until you multiply by per-event exclusive TIME.**

### C. Powering the A/B

13. **Never gate a verdict on one run per arm, or on the few windows a harness tails.** Run-level
    offset noise can be ±1.5 ms. One lever read "flat" (51.54 vs 51.60) on that basis; pooling every
    steady window across 4 runs per arm in both arm orders gave −2.45 ms with zero overlap.
14. **Two batches that disagree in SIGN mean the effect is under the floor.** −1.8 ms in order AB
    and +0.6 ms in order BA, against ~+3 ms of session-long thermal drift. Report neutral, ship
    opt-in, and do NOT average the two into a "win".
15. **Sub-millisecond levers need a within-process randomized A/B**, switching the lever per frame
    on a fixed pseudo-random sequence inside ONE process, discarding warm-up frames and computing
    blocked window deltas. Separate-process pairs cannot resolve them.
15a. **The "sub-millisecond" threshold in law 15 is a property of the HOST, and it drifts upward as
    a machine ages or as the arc's levers get smaller — re-derive it, do not inherit it.** Late in
    one arc the same host's within-*session* run-level drift reached **1.4× inside a single
    four-run block** (a base arm read 46.0 ms/f then 33.3 ms/f). An order-balanced 2v2 on a lever
    genuinely worth **−6.3 ms/f** returned two blocks that **disagreed in sign** (−8.4 / +4.0); the
    within-process paired form of the same lever, on the same binary, returned t = −118 and −154 over
    two runs. Once drift exceeds your levers, **cross-run A/B is inadmissible at every effect size**
    and the within-process instrument is not a special case for small levers — it is the only gate.
    Cheap diagnostic: print the control arm's absolute at the top and bottom of every block; if it
    moved by more than the effect you are chasing, the block is uninterpretable.
16. **A ballast probe is a SCREEN, not a gate.** An N=2 ballast returned a false null (ratio 0.06,
    t=0.25) for a site genuinely worth −0.75 ms. A null counts only if its CI excludes the effect.
17. **Never refactor a shared hot dispatch site during a perf phase without re-measuring the
    control.** The lever-OFF path *is* the control arm: adding a lambda wrapper around the dispatch
    inflated a measured win from −8.4% to −9.8%; an inlined census cost another ~2 ms/f. Use a local
    macro, keep the legacy call shape byte-identical, and re-measure against the pre-phase absolute.
17a. **…but do not ASSUME the probe is the regression either — measure it.** The converse mistake
    of law 17: a default-off `if (bool)` census hook in a hot SIMD path was blamed for a +2.6 ms
    regression, and rebuilding without it moved the arm **0.08 ms** — the whole regression belonged
    to the lever under test. Keep every hot-path probe behind a *compile-time* switch so this costs
    one rebuild to settle, and settle it before rewriting a diagnosis around it.
18. **Normalise per-batch costs by a REAL frame count, never a guessed one.** Assuming one upload
    per frame where the game issued 2.00 understated a cost by 57%.
19. **The unit of replication is the RUN, not the window — a pooled-window standard error is not
    the uncertainty of your A/B.** Within one run the steady level is very stable (per-run `se`
    0.14–0.30 ms over a dozen windows), but that level is set at process start and can vary 4 ms
    run to run. Three runs per arm, all with the same arm FIRST in every pair, produced a tightly
    grouped and apparently overwhelming −3.25 ms; flipping the pair order gave +0.68 ms. Same-order
    replication multiplies confidence in an artifact. Always balance arm order, report the
    per-order contrasts separately, and apply law 14 to their signs.
20. **A derivative arm must carry the SAME instrument set as its baseline.** Comparing an
    uninstrumented arm against an uninstrumented baseline while the *other* arm's flag silently
    auto-enables a profiler yielded a thread sensitivity of **2.08** — impossible for a serial
    thread chain, which is the tell that the baselines differed. Matched, the same thread measured
    1.15. A sensitivity above ~1 is a bug in the experiment, not a discovery.
21. **Busy CPU time on a thread and BLOCKED time on the same thread are different resources; a
    derivative spin measures only the former.** A thread ranked as the pole (sensitivity 1.15) was
    found to host a completely FREE ~11 ms/f block: deleting the blocking cross-thread round-trip
    it waited on — a workload-invariant exact deletion, freeing real time on both that thread and
    the worker it waited for — moved the frame ~0. When every thread is near-saturated, freeing a
    block frees a core but removes nothing from the critical path. Before building anything on top
    of "thread X is the pole", ask whether the candidate cost is X's OWN work or something X waits
    on, and price the latter with a deletion probe first.
22. **A corrupting ceiling probe is only a price with a WORKLOAD INVARIANT.** Deleting work in an
    incorrect-output arm changes the *guest's* control flow, so the arm can execute a different
    amount of downstream work — one such probe deleted a store and read 5 ms SLOWER, another
    returned sd=4.42. Pick an invariant the probe cannot touch (submitted packets/frame, draw
    batches/frame), read it on both arms in the same session, and only then quote a ceiling.

23. **Two levers that remove the SAME serialized latency do not add — gate them on a COMBINED arm.**
    A caller-side and a worker-side spin on one cross-thread round trip measured −1.23 and −0.95 ms/f
    standalone but **−1.14 together**, with each one's marginal value over the other roughly half its
    standalone value. Promoting on the sum would have overclaimed by ~60%. Whenever two levers touch
    the same serialized edge or the same thread handoff, add an A/B arm that alternates BOTH —
    exactly the shipped default against exactly the full rollback — and gate the promotion on that.
24. **A lever that buys wall time by BURNING CPU is host-shaped; ship it with a rollback and say so.**
    Spin-then-block waits are free on a machine with spare cores (6 physical / 12 logical, four busy
    runtime threads) and can be a regression on a 4-thread host, with no change to the code. Record
    the host's core count next to the measurement and re-run the combined gate on any new machine
    before trusting the default.


---

## §3 Record It

For every accepted optimization, one line in `PS2_PROJECT_STATE.md → Learned Patterns`:
`<hotspot> cost <N>% frame time, fixed with <what>, FPS <before> → <after>, baseline metric unchanged`.
If it changed an output metric even slightly: it is NOT an optimization — reclassify as a behavior
change and route it through the normal fix taxonomy.

For every rejected optimization premise, record the current-binary profile, payoff ceiling,
mechanism counters, reverse-order A/B distribution, normal-presentation evidence, prototype
disposition, and focused next mechanism in the phase fix log. Never summarize an internal counter
reduction as a performance win.

Cross-refs: verification ladder `10-agent-guardrails.md` §4; lever/kill-switch doctrine
`15b-gs-state-and-capture-ab.md` §3; lock model `16-runtime-concurrency-threading.md`; SIMD note
`04-runtime-syscalls-stubs.md` §6; build gate `SKILL.md` §4.
