# 17e — Performance Measurement Traps (I) — NAMING · CENSUSES · PRICING

> **Generic (game-agnostic).** Every entry is a way a measurement lied, caught in a real port.
> Read this WITH `17a-perf-measurement.md` (which defines the instruments) — `17a` says how to
> measure, this file says how measurement goes wrong.
>
> ⭐ **This file covers everything BEFORE you have a lever running: naming the pole (§1), censuses
> that move what they measure (§2), and pricing a lever before building it (§3).** The traps that
> bite once you are RUNNING the gate — the A/B itself, harness hygiene, and oracles that report
> "clean" because they never ran — moved to **`17f-ab-gate-and-oracle-traps.md`** (2026-08-17,
> G617). §6's one-screen summary below indexes BOTH files.
>
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

### 1.8 ⭐⭐⭐ HEADROOM GOES STALE AFTER A PROMOTION — AND IT HAS CHANGED THE POLE MORE THAN ONCE

§1.7 says a big win re-ranks the threads. The operational form of it: **the number that decides
whether your next lever can pay — `pole − runnerUp` — is invalidated by your own last promotion**,
and re-deriving it is one census run.

One arc landed three consecutive VU1 promotions worth ~1.8, ~5 and ~2 ms/f of that thread's time.
Each was real and each was measured on the thread. But the route's headroom was only ~1 ms/f, so
after the first one **VU1 crossed BELOW the other worker** and the frame stopped following it: a
mechanism reading **−23.7% of the pair loop** gated at **NULL (+0.023 ± 0.081)**. Two phases later
the same route had flipped from 12.4% to **0% VU1-poled**, which un-shelved every lever previously
parked for want of budget on the other unit.

- **Re-derive the pole on the SHIPPED binary at the top of every phase.** Not from last phase's
  table, and never from a table taken before a promotion that landed since.
- **Quote the paired gate in %, never the level** — two census runs summarise different window
  populations and their means are not comparable across sessions. Only the within-run derivative
  (what fraction of windows each unit poled) is quotable.
- A lever's prize is the **HEADROOM**, not the pole's busy time. Below the runner-up it buys nothing.

### 1.9 ⭐⭐⭐ A SHARE OF A SUBSYSTEM IS NOT A PRICE, AND A SUBSYSTEM'S WALL TIME CAPS EVERY LEVER IN IT

Two different errors with the same shape — a percentage read as if it were milliseconds.

**(a) A share is not a price.** "This stage is 70% of the backend" does not license "removing it
saves 70% of anything" until you know what the backend is in ms/f *of the frame*.

**(b) The subsystem's total wall time is the CEILING on every lever inside it — check it BEFORE
building.** One port cut the four largest leaves of one CPU replay lane by **−33%, −58%, −86% and
−21% `cyc/inside`** across three phases, every one exact and every one confirmed in the lane. **Every
frame gate after the first read null**, because the entire lane was **~1.1 ms/f of wall inside a
34 ms frame** — and a lane saving is additionally divided by the lane fan-out before it reaches the
frame.

> Before building: convert the target population's lane time to wall
> (`lane ms ÷ lanes ÷ frames`) and compare it to what your gate resolves (`17f §4.2a`). If the WHOLE
> subsystem is below that, no lever inside it can produce a frame result — **decide on that basis and
> say so in advance**, rather than reporting a surprised null afterwards.

⭐ **The corollary is the valuable half.** If the subsystem you have been optimising is small and the
thread's own time is large, **the time is somewhere you have not decomposed yet.** In that port,
`GS own = 30.70 ms/f` against a band replay of ~1.1 — which proved the thread's 21.4 ms/f "front"
interval was *not* the thing three phases had been optimising, and made decomposing it the only
remaining question on the route.

### 1.10 ⭐⭐⭐ A PC-SAMPLE BUCKET THAT CANNOT NAME A MODULE IS NOT "WAIT"

A sampling profiler that reports a large bucket with no symbol — or one attributed to a generic
`ntdll`/`kernel32` frame — is reporting **that it could not resolve the frame**, not that the thread
was blocked. Naming it "wait" and then designing a lever to remove the wait is designing against the
profiler's own failure mode.

**Resolve it or discard it:** get the module name (`/MAP`, a symbol server, a manual return-address
walk), or replace the inference with a *direct* measurement — a wall-clock bracket on the suspected
blocking edge, which also tells you the count. A bucket you cannot name is not evidence for either
answer.

---

## §2 CENSUSES AND INSTRUMENTS THAT MOVE WHAT THEY MEASURE

### 2.0 ⭐⭐⭐ A BUCKET THAT SPANS AN ADMISSION BOUNDARY IS TWO POPULATIONS, AND ITS MEAN DESCRIBES NEITHER (G617)

Once any fast path can REFUSE a work item, a census bucket that ignores the refusal averages two
loops with different per-unit prices, weighted invisibly. One row read "≈15% of the workload at
366–855 cyc/unit, and its items are small so rank it second"; split by admission it was **37.4% at
819 cyc/unit — the largest single item present**, at 5.3× the admitted path's price.

Two rules, both cheap:

1. **Put the admission in the census KEY.** Derive the leaf from the admission boolean, and hoist
   that boolean **above** the scope's loop mark, so one item cannot book its prologue to one bucket
   and its loop to another.
2. **Key a "shape"/state table on what SELECTS a kernel** — the sampler-enable bit, the format, the
   filter — never on the primitive/type enum. A table keyed on type let textured and untextured
   items share rows whenever the stale state matched.

⭐ **The detector is free and should be run on every such table: the shape rows' unit counts must sum
to the leaf's own.** Here they summed to **1.9×** it, in plain sight, for four phases.

⚠️ **And a cumulative census is only PAIRED if both arms are read at the same boundary.** If it
prints at fixed call counts, the two arms' final prints usually land on different ones — cumulative
columns (`share%`, total ms) are then incomparable. Read the longer run's *earlier* block instead.

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


---

## §6 THE ONE-SCREEN SUMMARY

| Before you… | …first |
|---|---|
| build ANY lever inside a subsystem | convert the SUBSYSTEM's total time to wall ms/f and compare it to what your gate resolves — if the whole subsystem is under that, no lever in it can pay (§1.9) |
| start a phase | re-derive the pole on the SHIPPED binary; your own last promotion invalidated the headroom (§1.8) |
| rank buckets from a census | check no bucket spans an ADMISSION boundary, and that the shape rows' counts sum to the leaf's (§2.0) |
| call a profiler bucket "wait" | resolve its module — an unnamed bucket is a profiler failure, not a blocked thread (§1.10) |
| carry a stage ranking to another population | don't — a stage's share belongs to the LEAF that implements it and to its hit rate (`17d §9a.2`) |
| profile a hot leaf | grep it for function-local `static`/`…On()` flag accessors first — each is a per-access TLS guard (`17d §9a.1`) |
| dismiss a memo whose hit rate is ~0 | ask why the key never repeats; a re-keying may be exact and halve the work (`17d §9a.3`) |
| rank an INTERPRETER's stages by deletion | don't — the deleted slot writes the guest's own induction variables (`17d §9b`) |
| report a NULL A/B | print how many times the candidate arm took the NEW path — exact levers can measure null by never diverging (`17f §4.8`) |
| compare two arms' captured output | run ON/ON′/OFF/OFF′ and read the PAIRED EXCESS; two runs of one binary do not agree (`17f §4.9`) |
| re-open a "refuted" lever | check whether the ROUTE or the PROBE that refuted it has since improved (`17b §2.x`) |
| rank defects to fix | rank by BLAST RADIUS, not firing rate; a rare fallback that mutates shared state is top-tier (`17b §2.y`) |
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

> ⭐ **§4 (running the A/B), §5 (harness hygiene), §6 (oracles that never ran) and §7 (weak-key
> reuse) are in `17f-ab-gate-and-oracle-traps.md`.** Section numbers there are unchanged, so every
> bare `§4.x`, `§5.x`, `§7.x` reference in the table ABOVE resolves in that file; bare `§1.x`,
> `§2.x`, `§3.x` resolve here.

