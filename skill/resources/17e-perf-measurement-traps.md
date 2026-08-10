# 17e — Performance Measurement Traps

> **Generic (game-agnostic).** Every entry is a way a measurement lied, caught in a real port.
> Read this WITH `17a-perf-measurement.md` (which defines the instruments) — `17a` says how to
> measure, this file says how measurement goes wrong.
> Companions: `17d-hot-loop-and-codegen-laws.md`, `17b-perf-levers.md`, `17c-perf-gs-pipeline.md`.

---

## §1 NAMING THE POLE

### 1.1 The frame is `max(threads)`, never `sum(buckets)`
Attribute by THREAD first. A per-bucket table inside one thread cannot tell you whether that thread
is the frame.

### 1.2 Derivative probes RANK poles; they are not efficiencies
Inject a fixed spin on a thread and read the frame delta. The nominal `1 µs × count` denominator
understates what a `steady_clock` poll loop really costs, so a computed "sensitivity" above 1.0 is an
artefact of the denominator, not a superlinear thread. **Read the probes as an ordering.**

### 1.3 A coin toss is not a ranking — when two probes are TIED, stop re-taking them
A port recorded its two workers' ranking as having "flipped sixteen times" across phases in which
**no code landed on the other thread at all**. It was never flipping: one probe read **+0.136** and
**+1.043 ms/f** on two runs of the *same binary* twenty minutes apart. The **within-probe spread
equalled the between-probe gap**, because the two threads were within a few percent of each other.

**Symptom:** a ranking that moves without cause, phase after phase.
**Diagnosis:** the quantities are tied.
**Fix:** measure **OCCUPANCY** — one thread's busy ms/f against the frame — which is stable where a
difference of two noisy deltas is not. Seven phases were scheduled against a coin toss.

### 1.4 A thread's SPAN is not its WORK
A worker reporting the largest `totalMs/f` can be the one with slack, if its span includes blocking
on a downstream thread. Split every occupancy number into *busy* vs *blocked* before ranking, and
cross-check with a deletion probe on the suspected block (§3.2).

### 1.5 A probe's spin length is PER EVENT, not per frame
A spin charged ~1,230×/frame at `=1500 µs` injects ~2.4 seconds per frame, collapses the run, and
**never reaches the measurement window at all** — the probe silently produces NO output rather than
an obviously wrong one. Use `=1` and let the event count do the multiplying.

### 1.6 A thread-level probe must HOLD its arm
Alternating an arm every single frame lets pipelined work leak across the transition. Hold the arm
for a block (≈16 frames) so each arm's frames are attributable.

### 1.7 A big win on the pole RE-RANKS the threads — most of it may not reach the frame
The mirror of §1.3/§1.4, and the one that decides what an arc does next. A lever removed **≈5.5 ms/f**
of the pole worker's busy time; the frame moved **≈1.9 ms/f**. Nothing was mis-measured: the thread
went from ~25 ms to ~20 ms against peers at ~24.8 and ~25.2, i.e. **it crossed under them**, and a
lever on the pole converts to frame time only until that happens (`17a` §2 law 7).

- **Compute `pole − runnerUp` BEFORE building**, state it as the phase's ceiling, and size the claim
  to it — not to the work you expect to remove.
- **Re-take the occupancy census immediately after any win over ~1 ms**, not at the start of the next
  phase. The arc's *next* target is chosen from the post-win ranking, and a plan written against the
  pre-win ranking will aim at a thread that now has slack.
- **Report the thread-level and frame-level results as two separate numbers**, always. "−5.5 ms of
  worker time, −1.9 ms of frame" is the honest sentence; either number alone is misleading, and a
  thread win with a flat frame is still worth promoting when it removes a floor — say which.
- Beware the arc-level consequence: an architectural plan justified by "this thread is 95% of the
  frame" (a translator, a rewrite, a parallelisation) can be **retired by its own first increment**,
  because that increment moves the thread off the pole. Re-derive the arc's arithmetic from the new
  census before continuing it.

---

## §2 CENSUSES AND INSTRUMENTS THAT MOVE WHAT THEY MEASURE

### 2.1 A census inflates its OWN bucket
Timers around a stage charge that stage for the clock reads. A backend census read a readback bucket
at 13.0 ms/f where an independent clock read 7.9. **Discount an armed census's own bucket, or clock
it independently.**

### 2.2 …and it can inflate a DIFFERENT THREAD's bucket enough to INVERT the ranking
The worst instance of §2.1. An attribution run armed a per-operation counter (counting 2.6 G
ops/frame) alongside the thread census and read the pole at **44.5 ms/f busy with its consumer
stalling 15.6 ms/f on it** — the exact opposite of the truth. The light census read the same thread
at **24.3 ms/f, consumer stall 0.0**.

**Rule: an attribution census must not be able to re-order the threads it is attributing.** Keep a
LIGHT set (thread busy-time + boundary counters only) and never mix per-op counters into it.

### 2.3 A default-OFF diagnostic must not be able to move the default path
Census counters added as members of a `thread_local` block shift every `thread_local` declared after
it — a data-placement change on the pole's own TLS block, and one such alignment was worth 3.48 ms/f.
**"It is behind a flag" covers the CODE, never the STORAGE.** Put diagnostic accumulators in
file-scope statics.

### 2.4 A census can DISABLE the mechanism it is measuring
Arming a census that forces a slow path means you measure the slow path. Check that the armed run
still takes the promoted code path (print a one-shot gate line naming the copy that ran).

### 2.5 A histogram whose INDEX collides is not an attribution
A coverage report keyed on a field that merely *correlates* with the decision put covered and
uncovered encodings in the same buckets, and read naively implied 11.5% of cycles against a true
4.61%. **Key a census on the DECISION you want to attribute**, not on a field near it.

### 2.5a A census reading ~0 may mean the shipped DEFAULT removed its subject
Once a lever is promoted, the census that found it reports nothing — the thing it times no longer
executes. A near-empty census then reads as "this vein was never worth anything", which is the exact
opposite of the truth. **Any census whose subject a promoted default can delete must say so at its
own definition site and require the rollback flag alongside it** (`CENSUS=1 NO_LEVER=1`). Same family
as a counter that has the arm itself as a conjunct, where an unarmed run reports `eligible=0` and
means "not armed".

### 2.5b A cumulative-over-run counter ÷ frames turns a BOOT cost into a permanent rate
A per-arm invariant census reported a CPU-fallback path at `0.03/frame` and a roadmap costed it at
"plausibly 0.1–0.5 ms/f of the shipping path". One run of the raw counter: the total was **52,
constant** across 600 consecutive prints — all of it at boot, and 52 ÷ ~1,500 attributed frames *is*
the 0.03. Steady-state cost zero. **A rate that never changes between windows is a fixed cost being
amortised, not a live one** — check two consecutive prints before building anything on it.

### 2.5c ⚠️⚠️ CONFIRM WHICH FIELD IS THE FRAME COUNT BEFORE DIVIDING BY IT
Every cumulative census has to be de-cumulated against a frame delta, and the per-frame line that
carries it may print **more than one plausible counter**. One port's frame line read
`n=4501 frame=9950 window=60`: `n` is the rendered-frame count (it advances by exactly `window` each
print) and `frame=` is the **input-script clock**, running **2.216× faster**. A whole phase's drain
table was built on `frame=` and came out **2.2× too small** — and the tell was that it contradicted
a counter printed **by the same process, in the same log**.

**A cross-instrument disagreement inside ONE process is almost always a denominator, not a
measurement.** Two checks, both free: (a) confirm the candidate advances by the window size between
consecutive prints; (b) reconcile the result against any independently-printed value for the same
quantity before believing it. Then put the de-cumulation in a script so the choice is made once.

### 2.6 Early-capped prints prove nothing
A probe that stops after N lines cannot tell you a rate, a distribution, or an absence. Use uncapped
counters and print periodically.

---

## §3 PRICING BEFORE BUILDING

### 3.1 Price a site's CALL RATE first
A profiler row names a symbol; with public-symbols-only PDBs every `static` folds into the preceding
exported one, so **the row is a hypothesis about which code owns the time, not an answer.** Two exact
levers built straight off such rows were both wrong: one targeted code that never executes (its
feature was default-OFF), the other a function running ~440×/f rather than the ~10k/f its own source
comment implied. **Install a call/visit counter first** — minutes against hours.
A visit census is also what PRICES a lever: 1.2 M walks measured 10.17 M old visits vs 7.64 M new,
i.e. ~940 atomic RMWs/f ≈ 6 µs/f — too small to build.

### 3.1a A CONSTANT per-item cost is fixed OVERHEAD, not work — check what BRACKETS the item
Before attributing a queued item's cost to the work it does, tabulate cost **per item** across items
of different size. A port measured its GPU backend's blocking edges at `render` **105 µs** for a batch
of **~418 draws**, one full-screen-triangle view build at **107 µs**, and another at **111 µs**. Two
orders of magnitude of work, one price: that uniformity *is* the finding. What the items had in common
was not their work but their **bracketing** — a validation query before and after each one.

The usual culprit on a **multi-threaded GL driver** (NVIDIA "Threaded Optimization" and equivalents):
a queued command is nearly free, but any entry point that must return a **server-side value** cannot
be queued — `glGetError`, `glCheckFramebufferStatus`, `glGetIntegerv`, `glGetTexImage`, `glFinish`,
`glClientWaitSync`. The caller blocks until the driver's own worker drains everything ahead of it.
That port was issuing **100 such queries per frame**, costing **4.6 ms/f = 43% of its pole thread's
entire blocked half**, with `bad=0` over ~360,000 calls — none had ever reported a problem. Eliding
them was **−1.49 ms/f, negative 4/4 on both estimators, 24 of 24 windows.**

Three lessons, in order of how much time they save:
- **A bucket's size is not evidence that its items are expensive.** That port spent one whole phase
  making a "1 ms" view build exact and measured under the floor; the item was 0.7 ms of *one*
  `while (glGetError() != GL_NO_ERROR) {}` drain and now costs 0.002 ms.
- **Removing the brackets can invalidate a whole planned work programme.** Three queued redundancy
  caches were on that roadmap; after the elision their subjects were 0.002–0.17 ms/f, below the
  instrument floor, and all three closed without a line written.
- **Expect the saving to be smaller than the census.** Some of what a sync absorbs is real pipeline
  latency that must be paid somewhere; there it re-landed on the one remaining true fence (a blocking
  readback rose 3.2 → 5.9 ms/f while the frame still fell 1.5). The paired A/B is the arbiter — the
  census only names the mechanism.

Eliding a validation query is exact only if you argue it **per site** (does anything downstream
consume the result? is the condition invariant?) and ship a **failsafe**: one real query every N items
that latches every elided check back on for the rest of the process if it ever trips. Then the failure
mode is "one window of undetected error, then the shipped behaviour", not silent corruption.

### 3.2 A deletion probe prices `min(work, frame − max(others))`
Deleting a block prices it only up to the point where another thread becomes the wall. A block worth
11 ms of one thread's time can measure a **ceiling of ~0** at the frame, and that is a real answer:
it is already off the critical path. Re-price a deletion probe when the pole moves.

### 3.3 A deletion probe that CORRUPTS output is not a price
Skipping a stage that produces geometry made the frame SLOWER (28.0 vs 25.5 ms/f) because the garbage
output changed the downstream workload. **Only probes that hold the workload invariant price
anything.** Check the invariant explicitly (same row/pixel/byte counts on both arms).

### 3.3a A deletion probe is valid on a CONSUMER edge and INVALID on a PRODUCER edge
This is the rule that tells you, *before you build it*, whether 3.3's invariant can hold at all.

- **Consumer edge** — a readback, a view/composite build, anything whose result is read by the CPU or
  sampled by a later draw. Deleting it leaves the guest's own state machine untouched, so the arms
  still run the same work and the number is a price. Two such probes on one arc converted **83%** and
  **62–66%** of their caller-side waits, both with the invariant holding to <1%.
- **Producer edge** — a render, an upload, anything whose output later feeds *dirty tracking,
  invalidation, scheduling or materialization*. Delete it and the engine has nothing to publish, so
  it stops generating work — and the probe reports an enormous win for having quietly stopped
  running the program. One arc measured **−24.2 ms/f (−94%, t = −51.8, 11/11 windows)** this way;
  the per-arm invariant showed the candidate building **35 draw calls against 829** and **1.3 flushes
  against 33.5**. Without the invariant that number would have aimed the next phase at a phantom
  worth more than the entire remaining gap.

Ask one question before building: *does anything downstream — including the engine's own next frame
— derive WORK from what this edge produces?* If yes, the only admissible probe **keeps the work and
deletes the WAIT**, which requires taking ownership of the caller's inputs (the caller typically
submits a reused/static buffer and rebuilds it immediately, so a non-blocking submit that keeps the
pointer is a data race, not a probe). That is a real mechanism, not a one-run ceiling — price its
COVERAGE first with the existing eligibility predicate.

**Print the invariant PER ARM, inside the same run**, next to the A/B line: entries into every edge,
the item/draw counts, and the fallback/slow-path counter. A cross-run invariant check cannot see an
arm-dependent collapse at all.

### 3.3b Caller WAIT ≈ worker WORK means the vein is concurrency, not latency
Before designing anything to "overlap the round trips", put the caller-side wait per edge and the
worker-side work per item in the **same run** and compare them. One arc found `wait 3.01–3.27 ms/f`
against `work 2.98–3.22 ms/f` on the same 20 items: the handoff residue had already been taken by an
earlier spin lever, and everything left was real work that must be made to run *concurrently* with
the blocked thread's own CPU. Batching or shortening the round trips would have bought exactly zero,
and the two are very different amounts of engineering.

**And do not treat a bucket as uniform.** In that same census one item cost **0.90–1.13 ms** while
the other 19 cost ~105 µs each — one item was 30% of the bucket. Census by subtype before choosing a
mechanism; the outlier is usually a different bug from the one the bucket's name suggests.

### 3.4 Measure the CEILING before building the mechanism
An upper bound costs an afternoon; a mechanism costs a phase. If the ceiling is ~0, stop.

### 3.4-0 ⭐⭐ A CALL-CLASS ARGUMENT IS NOT A PRICE — EVEN WHEN THE CALL CLASS IS REAL
The most seductive shape in perf work is a *correct mechanism story about what is inside a bucket*.
One rasterizer's bilinear filter ended in `std::lround`, which must round half away from zero —
which SSE's `cvtss2si` (round-to-nearest-**even**) does not — so the compiler could not fold it and
emitted a **CRT call, four per pixel over 363 M pixels**. An earlier phase in the same codebase had
*already* established that a CRT leaf row is a priceable call class **in that exact rasterizer**. An
exact branch-free replacement was derived and built.

**The ceiling probe that deleted the entire filter then measured 0.00 ms/f.** Every step of the
reasoning was true and the lever was worth nothing: at ~1,250 host cycles per pixel, the arithmetic
was not what the pixel was spending its time on — the scattered memory taps were.

**A mechanism story never substitutes for deleting the candidate work and reading the frame.** The
stronger the story's provenance ("we priced this class last phase"), the more it deserves the probe,
because that is exactly when it will be believed without one. Cost of the probe: one build.

### 3.4-0a Build the ceiling probe so it SEPARATES the surviving hypotheses
When a bucket has two plausible owners, one probe can decide between them if it deletes exactly one.
Same rasterizer: after the arithmetic was refuted, the survivors were "four swizzled texel taps" and
"the write round trip". A probe forcing point sampling cuts taps 4 → 1 and leaves the write path
untouched, so its paired delta attributes the bucket without building either lever. Design probes as
**discriminators**, not just as upper bounds.

### 3.4-0b ⭐⭐ A PARTIAL deletion bounds a FULL deletion from BELOW — check every probe's direction
The reflex is "a deletion probe is an upper bound on the lever", and it is wrong whenever the probe
deletes only *part* of the subject. The point-sampling probe above removes three of four texture taps
but **leaves the fourth still swizzled**, whereas the lever it was standing in for (a de-swizzled
whole-texture decode) makes **all four linear**. Its −1.37 ms/f is therefore a **lower** bound on the
lever, and the phase very nearly filed it as an upper one — which would have demoted a ≥27% lever to
"worth ~7%, don't bother".

Before quoting any probe as a bound, state which direction it bounds in and why: **does the probe
leave any of the subject's work still being done?** If yes, it bounds from below. (Related: §3.2's
`min(work, frame − max(others))`, which bounds from above for a *complete* deletion, and the
superlinear/linear distinction — a superlinear sensitivity curve bounds a removal from above, a
linear one is only a point estimate.)

### 3.4a PRICE ONE INSTANCE OF THE OVERHEAD — with an INFLATION probe, which is safe where a deletion probe is not
Whenever an arc rests on "mechanism X amortises overhead Y", somebody must eventually measure what
**one Y** costs — and it is astonishing how long that goes unmeasured. Five consecutive phases of one
arc optimised a block executor built to amortise an interpreter's per-item envelope, and none of them
had priced a single block entry.

The right instrument is an **inflation probe**: make the overhead happen N times more often while the
*work* stays byte-identical, then divide the frame delta by the extra count. In that arc the probe
halved every long block recursively — the same items executed in the same order with the same
coverage, only more trips round the loop head — and read **+0.360 ms/f, sign 4/4, over 190,653 extra
round trips per frame ⇒ 1.89 ns each**, about seven host cycles for the whole loop head plus a
noinline 10-argument call.

Two reasons to prefer inflation over deletion:
- **It cannot produce a §3.3/§3.3a artefact.** It removes no output, so nothing downstream — the
  guest included — can derive different work from it, and the per-arm workload invariant is trivially
  satisfiable (and should still be printed).
- **Its answer is a rate, not a bound**, so it prices every *future* lever in the same family at
  once. That 1.89 ns immediately said "the entire remaining envelope vein is 158 k × 1.89 =
  0.30 ms/f", which retired a chaining lever whose code was already written, an admission lever, and
  a minimum-run-length lever — none of them ever needed a gate run.

### 3.4b A PER-ITEM CENSUS CANNOT SHARE A RUN WITH A TIMING MEASUREMENT FOR A LEVER THAT CHANGES THE ITEM COUNT
The standard defence for leaving a census armed during a gate — "it is present in both arms, so it
cancels in the paired estimator" — holds only while the per-arm ITEM COUNT is invariant. A per-block
counter fires once per block; a coverage/admission lever's entire effect is *more blocks*; so the
instrument taxes the candidate arm and nothing else. One arc's invariant run read its lever at
**+0.350 ms/f** where four clean runs read **+0.121** — a 3× inflation of the very refutation it was
supposed to support. Take the workload invariant in its OWN run. Audit every `if (stat) note(...)`
hook against the question "does my lever change how often this fires?" — this is a superset of §2.1.

### 3.5 An upper-bound identity is not a hit rate
A duplicate-detection rate keyed on a weak identity sized a memo for three phases. Keyed on the state
a memo actually needs, the hit rate was **0.00%**. Compute the WEAK and the FULL identity in the same
binary and the same run, and split the failure by component — "both halves differ on every repeat" is
a structural closure, while "only the registers differ" would have been a narrower lead.
*Trap inside the trap:* an **OUTPUT** field in the key (one the operation itself overwrites) makes two
identical items miss on their predecessor's ending value. A negative from an over-specific key proves
nothing.

### 3.6 Safe item COUNT is not merge runway
Classify immediate successors and exact dependency classes before building a batching mechanism.
13,011 conservatively-eligible items had **zero adjacent pairs** — every adjacency belonged to a
commit point.

### 3.7 Steady CONSUMERS before hot-call optimisation
An aggregate helper count can be dominated by scene construction. A helper with 3.99 M exact
comparisons had **zero post-warm consumers across 1,200 ticks** — no steady-FPS premise at all. Print
`used(...)` per arm and never interpret periods with no consumer or mixed arms.

### 3.8 A two-point linear fit across two populations is NOT a decomposition
Fitting `cost = a + b·n` through two population means implied **3.2 µs of fixed per-item overhead**
(~4 ms/f) in a place no instrument had looked. Measured directly with three clock reads, the intercept
was **0.21 µs — a 15× overestimate**. "Fixed overhead amortised differently" and "different work per
unit" explain the same two points *exactly*. When a fit implies a prize worth a phase, spend twenty
minutes measuring the intercept.

### 3.9 Removing an UNCONTENDED lock is not free
A per-item mutex + `notify_all` on a queue whose consumer runs ~1,950 items ahead and never sleeps
looked like free money. Skipping both read **+0.528 ms/f**: an uncontended mutex is a couple of
cache-hot atomics and `notify_all` on an empty CV is a no-op, while the replacement needed a `seq_cst`
counter load and a stronger store — two full barriers per item on the pole. **Price the barriers the
removal ADDS**, and confirm with the site's own timer.

---

## §4 RUNNING THE A/B

### 4.1 Cross-run A/B can be DEAD on a host
When run-to-run drift exceeds the effect, only a **within-process randomized A/B** can measure it.
Randomize the arm inside one process and difference away the level.

### 4.2 A paired A/B cancels the LEVEL, not the effect SIZE
The paired form removes run-level drift; it does not make a small effect big. Power still matters:
two runs of the same binary 20 minutes apart read **−0.12 (t = −1.1)** and **−0.75 (t = −11.9)**, and
the null run's own standard error was 3× larger. **Promote — and refute — only on a run whose own
`se` is in the usual band. Re-run before believing a null.**

### 4.2a The DRIFT-CONTAMINATED estimator and the BLOCKED estimator can disagree — the blocked one wins
A paired instrument usually reports two things: a raw pooled `delta` over the whole run, and a
**blocked** estimate that differences each short window's two arm means before averaging. The raw
delta still carries within-run drift; the blocked one is what the pairing was built for. One lever
read a raw delta that was negative in **4 runs out of 4** (−0.110 / −0.036 / −0.067 / −0.453) while
its blocked estimate flipped sign (−0.049 / **+0.089** / **+0.005** / −0.064) with |t| ≤ 0.96 and a
favouring count indistinguishable from chance. **Verdict: neutral.** Sign consistency on the
contaminated estimator is not evidence, and quoting "negative 4/4" would have promoted noise.

*Corollary — price the lever before spending the runs.* That same lever removed a **measured**
0.25 ms/f of work on an edge whose conversion had already been measured at 62%, i.e. ≈0.16 ms/f of
frame — below the instrument's floor, and computable from the census before four A/B runs were
spent. Multiply the removed work by the edge's known conversion first; if the product is under your
floor, you already have the answer.

### 4.2b A BIMODAL CONTROL makes a small paired gate read any sign you like — gate against BOTH modes

A paired A/B assumes the two arms are sampled from distributions that differ only by the lever. When
the **control** arm is bimodal and the candidate is not, a 2-pair or 4-pair gate is really sampling
*which mode the control visited*, and its sign is close to a coin toss.

Worked example (DC2 G528). Six runs of the identical rollback configuration on one route produced
**36.99 · 37.19 ‖ 42.01 · 42.21 · 42.37 · 42.61** — two clusters 5.6 ms/f (14%) apart — while the
candidate produced a single cluster. The first confirmation gate happened to draw one control run
from each cluster and reported **mean −0.03%, 1/2 negative**, with one pair at **+6.92%**. Two more
pairs, one of which caught the control in its *fast* mode, read **−4.55% and −5.42%**. Pooled: 9 of
10 paired runs negative.

**The decisive test is not more pairs, it is a pair against the control's FAVOURABLE mode.** If the
candidate beats the control when the control is at its best, mode selection cannot explain the win.
Diagnose the shape first: sort each arm's per-run medians and look for clustering before averaging —
a bimodal arm is visible in six numbers and invisible in a mean.

Corollary: **"route X is the stable one" expires.** The same route that repeated to 0.36 ms/f in one
gate spanned 14.6% two hours later. Stability is a property of the host at that moment. Re-derive it
from the run medians in the gate you are actually quoting, never from a previous phase's claim.

### 4.3 An absolute ms/f from a LOADED host is not a baseline
Mid-session readings rose to 27–39 ms/f and drifted upward *inside a single run* (29.9 → 39.4 on
identical frames) with a browser and editors resident. Quiet-host runs on the same binary read
25.15–26.03, flat. **Check the trend WITHIN the run before believing any level**; this is exactly what
the paired within-process form exists for.

### 4.4 Ballast is a SCREEN, not a gate
Repeating a site's work N times measures the site's critical-path SENSITIVITY — multiply an isolated
saving by it before promising frame payoff. It screens out insensitive sites; it does not promote.

### 4.5 An in-binary switch is not a control for a SHAPE change
A runtime selector pollutes both arms' code layout. Test code/data **shape** changes against a saved
control binary, or against a compile-time loop COPY (`17d §6.4`).

### 4.6 A "neutral" verdict EXPIRES with the pole — but check the arm is still REACHABLE first
A lever measured on an insensitive thread was never measured; re-price shelved levers when the pole
moves, and expect **both** signs (`17a` §2 law 7d).

**But the re-price can be invalid in the same way the original was.** The worked case here is a
cautionary one, because the skill carried its wrong conclusion for twelve phases. A shelved
run-executor lever was re-priced on the new pole and read **+0.34 ms/f SLOWER, 6/6 windows** — a
clean, well-powered, entirely consistent result, recorded as *"an old neutral was hiding a real
regression"* and promoted into the NO-GO table, where it became the gate on an architectural arc.

It was measuring nothing. The lever's arms sat below an `if/else if` loop-copy arm whose predicate had
been **promoted to default-true two phases after the lever was shelved**, so the candidate flag could
no longer select the candidate copy — it only dropped the run onto an older, slower copy. Both arms
were pre-promotion bodies. Rebuilt correctly, the mechanism was worth **−1.91 ms/f** and moved the
pole off that thread entirely.

**So the re-price protocol is:** (a) confirm the arm can still be SELECTED — re-read the selector
chain (`17d` §6.4a) and require a proof-of-selection line from **both** arms of the paired run
(`17d` §7); (b) confirm the CONTROL arm is the current shipped default, not a body that has been
superseded since; only then (c) believe the sign. A consistent, high-`t` result from an arm that is
not the arm you think it is looks exactly like a real finding.

### 4.7 Verify by FULL-FRAME DISTRIBUTION, never a single golden sample
Aggregate pixel counts are blind to dropouts over opaque scenes. Review multiple frames and check
edges/background as well as the main subject.

---

## §5 HARNESS HYGIENE

### 5.1 A flag not in the harness's CLEAR-LIST leaks into every later run
A harness that saves/restores only the variables it lists will let a census flag armed once persist
for the rest of the session — running later A/Bs on a different, slower code path — while the
harness's own environment echo looks perfectly clean, **because it echoes only what THIS invocation
set. The echo proves what was set, never what was already there.** Add every census/verify/proof flag
to the clear-list in the same edit that adds the flag.

### 5.2 Invoke a perf harness IN-PROCESS, never through a nested shell
`powershell -ExecutionPolicy Bypass -File script.ps1 -Set 'A=1','B=1','C=1'` is a **native command
line**: it flattens the array argument into the SINGLE string `A=1,B=1,C=1`, so the script sets only
`A` — to the truthy garbage value `1,B=1,C=1` — and silently drops every later flag, while the
harness's own echo still looks plausible. A nested `powershell -File` can also no-op entirely and
leave you reading a stale log.
**Call the script directly in the current session**: `& 'path\script.ps1' -Set @('A=1','B=1')`.

### 5.3 An up-to-date target does NOT relink
`cmake --build` reports success without relinking, so `$env:LINK='/MAP:…'` silently produces no map.
Force a recompile before any link-time diagnostic.

### 5.4 Never pixel-diff same-numbered frames across two runs
Frame filenames are host ticks, not guest frames. Cross-check the route reached the intended mode via
a state trace before comparing anything.

### 5.5 ⭐⭐⭐ WHEN A LEVER CHANGES SPEED, EVERY CLOCK INDEXED BY ELAPSED FRAMES DESYNCS
On a fixed-wall-clock scripted route the faster arm **plays more of the game**. Both the perf number
and the pixel comparison are then computed over *different scenes*, and both lie — in opposite
directions.

*Perf:* one arm reached 6,452 frames where the other reached 5,847 in the same 200 s. Three ways of
computing the same delta: naive whole-run mean **−9.4%**; bucketed by script position, all common
buckets **−10.7%** (inflated — the extra tail buckets are ones the slow arm barely entered, 32
samples against 273); bucketed **and restricted to buckets with matched sample counts** → **−9.9%**,
the only honest one.

*Pixels:* a per-tick comparison of the same cutscene showed median mean-byte **−9.76** with 145/176
frames differing — which reads as a severe rendering regression. The faster arm was simply rendering
**1.42× more guest frames per tick**, i.e. showing a *later moment*. Fitting the time scale (best
fit **1.450**, matching the measured ratio) collapsed it to median **+0.23**, mean **−0.20**,
21/122 — no divergence at all.

**Method.** Find the clock the *script* uses (the pad-replay frame index), not the guest frame
counter or the host tick:
1. bucket frame times by script position (e.g. `scriptFrame // 200`);
2. **drop any bucket where `min(nA,nB) < 0.95 · max(nA,nB)`**;
3. report the pooled mean, the fraction of buckets favouring the candidate, and the per-run means so
   their ranges can be checked for overlap;
4. for pixels, fit the scale factor between the two dump sequences and compare **fitted pairs**,
   reporting the signed mean (bias) as well as the absolute.

If the arms cannot be aligned, the route cannot decide the lever — say so rather than quoting the
naive number.

### 5.6 ⭐⭐⭐ "TAG X PRECEDES 63% OF SPIKES" IS NOTHING WITHOUT THE BASE RATE
Hunting isolated frame spikes, one log tag preceded **55 of 87** of them. Decisive-looking — and
wrong: the run emitted 2,505 of those lines over 3,525 frames (**0.71/frame**), so ~50% of *any*
frame class has one before it. The control settled it outright: the arm that emits **zero** such
lines had **86** spikes against the other's 87. Same spikes, unrelated mechanism.

**Always compute enrichment against a normal-frame control:**
`enrichment = (occurrences per SPIKE frame) ÷ (occurrences per NORMAL frame)`, and report both
columns. Doing that left exactly one enriched tag — and it covered 7 of 86 spikes, so the honest
conclusion was **"~90% of these spikes emit nothing; no shipped instrument covers them"**, not a
false attribution.
⚠️ Corollary: a tag firing thousands of times per run is a **background process**, not an **event**.
Spike causes are rare by construction — rank candidates by *rarity × enrichment*, never raw count.

### 5.7 ⚠️ A BUNDLE OF FLAGS FLIPPED IN ONE EDIT IS ONE LEVER WITH N UNKNOWNS
Four residency flags were promoted together on a **−22.0%** route-aligned result. A parallel session
had independently measured one of them, **alone**, as "pixel-exact but slightly *slower*" on that
same route. Both can be true: three positives can carry a negative. A bundle result licenses the
bundle and nothing else — **re-gate any member you intend to rely on, cite, or build upon
individually.**

---

## §5.4 AN INSTRUMENT WHOSE FAILURE LOOKS LIKE ONE OF ITS ANSWERS

**A ballast the implementation may legally DISCARD is not an instrument.** A derivative probe injects
a known cost and divides the frame's response by it. If the injected work can be optimised away by a
compiler, a driver or the hardware, the probe injects nothing and reads **sensitivity ≈ 0** — which is
also the reading for a genuine "this unit has slack" verdict. The probe then cannot be falsified, and
the phase concludes the opposite of the truth.

Worked example (DC2 G496). The proposed GPU ballast was to re-issue the geometry with
`glColorMask(0,0,0,0)`, `glDepthMask(GL_FALSE)` and the depth test disabled — "the GPU redoes the
full fragment work and not one byte changes". Windows GL drivers are documented (and were already
documented *inside that project's own renderer*) to **elide a fragment invocation whose only
framebuffer output is fully masked**. The admissible shape was an **identity blend** —
`glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE)` ⇒ `dst = 0*src + 1*dst = dst` — with depth
writes masked: every fragment is rasterized, shaded, blended and **written**, so nothing can elide it,
and the byte written is the byte that was already there. It measured 0.70, not 0.

General forms of the same trap: a CPU ballast the optimiser can hoist or dead-code away (sink it
through a `volatile` or an atomic), a memory-traffic ballast that the cache absorbs (stride past the
cache), and any "idempotent repeat" whose second execution the target is free to skip.

**The rule:** before building any probe, ask *"if this silently did nothing, what would it print?"*
If the answer is one of the readings you intend to act on, redesign it.

### §5.4b AN ORACLE MUST ASSERT THE INVARIANT, NOT THE ABSENCE OF SUBSEQUENT WORK

When the lever's whole purpose is **to stop doing work that was not needed**, the tempting oracle is
"run the new path, then run the old path, and assert the old path finds nothing left to do." That
oracle is guaranteed to fail, and it fails **exactly in proportion to how well the lever works.**

*"The old path would still do something"* and *"the new path skipped something REQUIRED"* are
different propositions. Only the second is a defect. An oracle that cannot tell them apart reports
100% of the intended saving as a correctness failure.

Worked example (DC2 G528). The lever scopes a whole-state publication edge to the ranges the
consuming batch actually touches. Its first oracle ran the whole-state publish afterwards and counted
the work it still did: **`miss(disp=344 depth=344)` on 2,000 batches** — a 17% "failure rate" that
was entirely the disjoint batches correctly leaving a display readback coalesced, which is the
documented, intended behaviour of the very function being called.

The fix is to state the safety property and test *it*: the consumer reads and writes exactly the
pages in its own read/write sets, so a publication was required **iff a region the producer still
owns INTERSECTS those pages**. Re-stated that way — enumerate every still-pending producer-owned
region and test overlap against the subject's own ranges, counting an unresolvable range as a miss —
the same run read **0/0/0/0**.

**The rule:** an oracle for a "do less" lever must quantify over the SUBJECT's requirements, never
over the old path's behaviour. Write down the invariant in one sentence before writing the check; if
the sentence mentions the old code rather than the data, the oracle is wrong.

Corollary, and cheap: report the arm-selection counters next to the miss counts
(`scoped / whole / unknown / null`). A lever that fails closed on most calls gates at zero and is
indistinguishable from a broken one; G528's `scoped=2000 whole=0 unknown=0 null=0` is what proves the
0/0/0/0 was earned on the full population rather than on a handful of calls.

### §5.4a THE DETERMINISM CONTROL — before you believe any A-vs-B artefact diff

A byte-identical output comparison is only a parity oracle if the pipeline is deterministic at the
granularity you are comparing. Run the **same binary with the same flags twice** and diff those first.

DC2 G496: candidate-vs-rollback frame dumps differed on **99 of 103** frames — and the same-config
control differed on **97 of 103**, because the routes are wall-clock driven and the animation phase at
a given dump tick is not reproducible. The admissible test is the **quantile-matched distribution** of
a per-frame scalar (there, `PixelNonZero`) over every captured frame, with the requirement that the
arm difference sits inside the control's spread. It did: meanΔ 69 px between arms against 111 px
between two identical runs.

## §6 THE ONE-SCREEN SUMMARY

| Before you… | …first |
|---|---|
| rank threads | take the LIGHT occupancy census; probes are secondary (§1.3, §2.2) |
| trust a profiler row | install a call/visit counter (§3.1) |
| build a memo/cache | compute the FULL identity's hit rate (§3.5) |
| build a batcher | classify adjacency and dependency (§3.6) |
| build a DELETION ceiling probe | classify the edge PRODUCER-or-CONSUMER, and print the invariant PER ARM (§3.3a) |
| design an "overlap the round trips" mechanism | put caller WAIT and worker WORK in the same run — if they are equal there is no latency left, only concurrency (§3.3b) |
| attack a named bucket | census it by SUBTYPE; one item is often 30% of it (§3.3b) |
| attribute a queued item's cost to its WORK | tabulate cost PER ITEM across items of different size — a constant price is fixed overhead, and on a threaded GL driver it is usually a value-returning query (§3.1a) |
| conclude a vein is worthless from a ~0 census | check whether a promoted default already REMOVED the census's subject (§2.5a) |
| build on a small per-frame rate | check two consecutive prints — a cumulative total ÷ frames turns a boot cost into a permanent rate (§2.5b) |
| believe a raw paired `delta` | check the BLOCKED estimator agrees in sign (§4.2a) |
| spend four A/B runs | multiply the removed work by the edge's known conversion — if it is under the floor you already have the answer (§4.2a) |
| build a pool | check who FREES the buffers (`17d §8`) |
| remove a lock | price the barriers the removal adds (§3.9) |
| believe a null | re-run and check the run's own `se` (§4.2) |
| believe a level | check the trend within the run (§4.3) |
| believe a fit | measure the intercept directly (§3.8) |
| re-price a shelved lever | confirm its arm can still be SELECTED, and that the control is today's default (§4.6, `17d §6.4a`) |
| size a claim | compute `pole − runnerUp`; that is the ceiling, not the work removed (§1.7) |
| plan the next phase after a win | re-take the census — the win may have moved the pole (§1.7) |
| promote | four paired runs, quote mean + sign count; then compile out every copy the default path cannot select and RE-TAKE the A/B (`17d §3.1`) |
| build ANY ballast/probe | ask "if this silently did nothing, what would it print?" — if that is one of your intended answers, redesign it (§5.4) |
| believe an output diff between two arms | run the SAME-CONFIG control first; if it differs too, compare quantile-matched distributions instead (§5.4a) |
| predict a frame gain from removing work on one unit | ask which CO-POLE absorbs it — when two poles sit within ~10% of each other, halving one edge buys almost nothing (§1.7) |
| price a call/query by its COUNT | split it by whether the call can be QUEUED: a command-buffer write is ~10 ns, a value-returning driver query is tens of µs — 1000x apart (§3.1a) |
| infer "not the bottleneck" from low UTILISATION | take the derivative. A unit can be 94% idle and still ~1:1 on the critical path when nothing overlaps it (§1.3) |
| promote a lever that changes WHO OWNS pixels (residency/authority/admission) | gate it on a PLAYER-MOTION route (jump/dash/animation change), not only scripted-camera and static screens — that class is invisible to them (`17 §1 Law 0`) |
| see a non-zero `invariant` / `escaped` / `conflict` / `stale` counter | treat it as a FAILED CORRECTNESS GATE, not as the mechanism of a slowdown (`17 §1 Law 0a`) |
| compare two arms of a lever that CHANGES SPEED on a scripted route | align on the SCRIPT's clock and drop unmatched buckets; for pixels, fit the time scale first (§5.5) |
| attribute spikes to a log tag | divide by the NORMAL-frame base rate; a 63% hit rate against a 50% base rate is noise (§5.6) |
| promote a BUNDLE of flags | the result licenses the bundle only — re-gate any member individually before relying on it (§5.7) |
| partially admit one member of a VRAM ALIAS FAMILY | don't: splitting ownership across an alias family made both halves escape and cost 3x more than admitting all of them (`17c`) |

---

## §6 ORACLES THAT NEVER RAN — the failure mode that reports "clean"

An exactness oracle is a *conditional* instrument: it compares only the items its activation
predicate selects. Every way that predicate can select **nothing** produces the same output as a
perfect pass. One port hit **four distinct instances in a single arc**, each costing a run.

### 6.1 ⭐⭐⭐ The optimization under test CHANGED the activation predicate
The oracle was set to fire on "batches with ≥64 entries", to skip the cheap startup traffic and
reach the dense population. It never fired. **Because the residency lever being tested coalesces
that target into ONE-ENTRY GPU batches** — so the very success of the optimization made the
oracle's size trigger unsatisfiable, and the census reported nothing rather than an error.

The fix was to re-key the trigger on something the lever cannot move (a *late-route attempt count*:
compare after the 10,000th batch, regardless of size).

**Rule: an oracle's activation predicate must be expressed in a quantity the lever under test cannot
change.** Batch size, draw count, buffer occupancy and path-taken flags are all things a lever
routinely alters. Wall-clock position, route position, and cumulative attempt counts are not.

### 6.2 The oracle's own plumbing DISABLED the mechanism it was comparing
Twice, in the same arc:
- the diagnostic switch that enabled the comparison **disabled GPU residency globally**, so the
  "GPU" arm was really a CPU arm and every comparison was CPU-vs-CPU;
- after that was fixed, a second switch still **forced every RTT batch back to the CPU on GPU
  success**, even on batches the oracle had skipped.

Both times the output looked plausible. **Check that an armed oracle run still takes the promoted
path** (`§2.4`) by printing a one-shot line naming the code path that actually executed, and assert
that the comparison count is non-zero before reading any verdict.

### 6.3 The oracle refused the batches by CONTRACT and reported zero comparisons
A colour-only RTT oracle was pointed at a target whose late traffic carries **guest depth**. The
oracle is specified to refuse depth batches, so it executed, refused everything, and printed a
comparison count of **zero** — not an error, and not a pass. Recognised correctly here ("comparison
count stays zero by design — no false pass"), but only because the count was printed.

### 6.4 The verifier's own writes contaminated the comparison
A candidate's early loads read RAM that the *generic* arm's tail stores had already modified, so the
two arms did not start from identical state and the oracle reported a mismatch that was an artefact.
Isolating the two exact store targets fixed it — **without** copying 16 KB per hit.

### 6.5 THE STANDING RULE
> **Print the comparison COUNT beside every `bad=` figure, and treat `compared=0` as a FAILED run.**

`bad=0 compared=0` and `bad=0 compared=1,600,000,000` are the same string minus the count, and only
one of them is evidence. This is the oracle-side twin of "a null A/B needs a path counter proving
the arms diverged".

---

## §7 A WEAK-KEY REUSE RATE IS NOT A MEMOIZATION OPPORTUNITY

A census reported same-frame state reuse of **26–28%** and previous-frame reuse of **14–33%** on a
heavy route, explicitly noted as reversing the lean baseline's premise — a strong-looking case for
memoizing the executor.

The full-identity census (hashing **two 16 KB state images plus registers** per invocation, and
verifying that identical pre-keys really do produce identical post-state) then found **zero valid
same-frame repeats**. The 26–28% was entirely an artefact of a key that ignored most of the state.

**Rule: a reuse rate is only actionable at the granularity of the FULL key you would have to compare
at runtime.** Before designing a cache, take the census with the complete key — including the state
you are hoping does not matter — and require it to *also* verify that equal keys imply equal
results. A weak key inflates the rate and a memo built on it is a correctness bug, not a speedup.

Related: a rate is not a price (`§3`), and a partial key is the same error as a colliding histogram
index (`§2.5`).
| build the 2nd/3rd implementation of a refuted lever | first check whether all arms cost the SAME — identical penalties mean the ADMISSION TEST is the cost, not the body (`17d §11`) |
| read `bad=0` from an oracle | read the COMPARISON COUNT beside it; `compared=0` is a failed run, not a pass (§6.5) |
| set an oracle's activation trigger | express it in a quantity the LEVER CANNOT CHANGE — the lever coalesced batches to 1 entry and the "size >= 64" trigger never fired (§6.1) |
| design a cache from a reuse rate | re-take it with the FULL runtime key; a 26-28% weak-key rate went to ZERO under full identity (§7) |
| add a GPU residency slot | enumerate every CONSUMER's index bound first; check `waves ~= materializations`, which means the content dies on arrival (`17c §4.1`) |
