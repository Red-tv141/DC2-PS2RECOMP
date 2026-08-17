# 17f — Performance Measurement Traps (II) — THE A/B · HARNESS · ORACLES

> **Generic (game-agnostic).** The second half of `17e-perf-measurement-traps.md`, split out
> 2026-08-17 (G617). `17e` covers everything before a lever runs — naming the pole, censuses,
> pricing. **This file covers what goes wrong once you are RUNNING the gate.** Section numbers are
> carried over unchanged (`§4`, `§5`, `§6`, `§7`), so existing cross-references still resolve.
>
> | you are about to… | read |
> |---|---|
> | attribute frames to an arm | **§4.0** — the arm must be sampled on the FRAME's schedule; **a branch is a conjunct** |
> | read a gate that is still running | **§4.0b** — an early print lies with enormous confidence; treat `w < 10` as no result |
> | build a lever inside one subsystem | **§4.0c** — the subsystem's total wall time is the CEILING; check it BEFORE building |
> | believe a raw paired delta | **§4.2a** — the blocked estimator must agree in SIGN |
> | trust any `bad=0` / "clean" | **§6** — four distinct ways an oracle reports clean because it never ran |
> | build a memo off a reuse rate | **§7** — a weak-key reuse rate is not a memoization opportunity |
> | add a flag to a harness | **§5** — it is restored only if it is in the clear-list |
>
> ⛔⛔ **The single most expensive lesson in this file: `bad=0` with `checks=0` is a FAILED RUN, not
> a proof.** Always print coverage beside the defect count.
>
> Companions: `17e-perf-measurement-traps.md` (its §6 one-screen summary indexes both files) ·
> `17a-perf-measurement.md` · `17d-hot-loop-and-codegen-laws.md`.

> ⚠️ **`§1` (naming the pole), `§2` (censuses) and `§3` (pricing before building) are in
> `17e-perf-measurement-traps.md`.** Section numbers are unchanged, so every `§2.4`, `§3.1`
> reference below resolves there.

---

## §4 RUNNING THE A/B

### 4.0 ⭐⭐⭐ THE ARM MUST BE SAMPLED ON THE **FRAME'S** SCHEDULE — AND A BRANCH IS A CONJUNCT (G615/G617)

A within-process randomized A/B attributes whole *frames* (or windows) to an arm. The arm register
is therefore only meaningful if the code that READS it runs for the frame's whole population. Two
successive phases got this wrong in two different ways:

- **G615** wrote `state && … && armOn()`. C++ short-circuits, so `armOn()` was reached only on draws
  inside the lever's footprint. Reading: `used(0=4175 1=119 2=147 3=0)` with `blocked(w=4)` — 94% of
  intervals saw NEITHER arm and were dropped. Fix: read the arm FIRST, AND the state terms onto it.
- **G617** obeyed that literally — each arm was the first statement of its own branch — and lost
  **96%** of intervals anyway (`used(0=3108 …)`, `blocked(w=2)`), because the branch it sat in
  (`else { /* untextured */ }`) *was* the lever's footprint. Fix: hoist the read above the branch,
  to the top of the dispatch function every member of the population enters. Dropped intervals went
  **96% → 4%**, the estimator from 2 windows to 28.

> **The rule:** place the arm read on a path taken by every member of the population the frames are
> made of, then AND every state term — including the branch condition — onto the resulting `bool`.
> **Symptom to grep for: a large `used(0=…)` beside a small `blocked(w=…)`.**

### 4.0b ⛔⛔ AN EARLY PRINT IS NOT A MEASUREMENT, AND IT LIES WITH ENORMOUS CONFIDENCE (G617)

If the route has a transient (a load spike, an entry burst), whichever arm occupies the first
windows reads catastrophically, and the blocked `t` is computed from **one pair difference** — so it
can be arbitrarily large by chance. Measured on one route, twice, in the same phase:

| print | w | raw Δ | blocked Δ | t |
|---|---:|---:|---:|---:|
| first | 2 | +5.774 | +5.586 | **+16.53** |
| … | 6 | +2.175 | −0.018 | −0.01 |
| final | 28 | **−0.368** | −1.001 | −0.93 |

A second leg of the same lever printed **+40.43% at t = 138.82** on its first window pair and ended
at −0.631. **Read only the LAST print of a COMPLETED leg; treat `w < 10` as no result.** A huge `|t|`
over a handful of windows means the estimator has no data, not that the effect is certain.

### 4.0c ⭐⭐⭐ THE SUBSYSTEM'S TOTAL WALL TIME IS THE CEILING — CHECK IT BEFORE BUILDING (G617)

`cyc/inside`-style rankings order work *inside* a subsystem; they say nothing about whether the
subsystem can move the frame. Three consecutive phases cut the four largest leaves of one CPU replay
lane by 33%, 58%, 86% and 21% — every one exact and confirmed in the lane — and **every frame gate
read null**, because the entire lane was ~1.1 ms/f of wall inside a 34 ms frame, and a lane saving
is additionally divided by the lane fan-out before it reaches the frame.

> Before building: convert the target population's lane time to wall
> (`LOOP ms ÷ lanes ÷ frames`) and compare it against what your gate resolves (§4.2). If the WHOLE
> subsystem is below that, no lever inside it can produce a frame result — decide on that basis and
> **say so in advance**, rather than reporting a surprised null afterwards. The corollary is the
> valuable half: if the subsystem is small and the thread's time is large, **the time is somewhere
> you have not decomposed yet.**

### 4.1 Cross-run A/B can be DEAD on a host
When run-to-run drift exceeds the effect, only a **within-process randomized A/B** can measure it.
Randomize the arm inside one process and difference away the level.

### 4.1b ⭐⭐⭐ SCENE-pairing is not SESSION-pairing — a COMPILE-TIME lever needs a sub-timer (G602)

Some levers cannot have a runtime arm: a pure code-SHAPE change (outlining, spill/frame layout,
inlining) can only be selected at compile time, because a runtime arm would be a **second copy of
the hot code inside the same function**, which §3.1 of `17d` measured at **1.35 ms/f for 6,196 bytes
nothing ever branches to**. So the two arms are two BINARIES, run order-balanced A B B A.

It is tempting to think that pairing **window-by-window on a scripted route** rescues this, since the
scene is a fixed function of the frame counter. **It does not.** Window pairing removes the *scene*
variance; the arms are still two processes minutes apart, so the *session* drift §4.1 describes is
untouched — and that drift is exactly what a within-process randomized arm exists to cancel.
Measured, on one route, one pair of binaries, `kicks/f` agreeing to **0.002%**:

| estimator | block A | block B | verdict |
|---|---:|---:|---|
| `avgFrameMs`, paired by window | **+0.063** (14/25) | **−1.224** (23/25) | blocks disagree in SIGN |
| the layer's **thread-local sub-timer** | **−0.924** (25/25, t −14.0) | **−1.048** (25/25, t −16.7) | agree, `se` 0.051 |

- **Gate a code-shape lever on a thread-local sub-timer**, which measures CPU *inside* the changed
  code rather than the wall of the frame. Convert to frame time afterwards via the headroom model
  (`frame ≈ max(threads)`), and say that is what you did.
- **Require both order blocks to agree in sign.** A pooled `t` over two disagreeing blocks is an
  artefact of pooling, not a result — the same data pooled to `t = −3.73`, which looks conclusive.
- **Do not re-bracket the window after the pre-registered one fails.** The same phase's second route
  read +0.31 on the window its own survey names, and −0.28 with both blocks agreeing on a wider one
  chosen afterwards. That is a garden of forking paths; report it as context.
- **Quote the workload denominator in the direction it moved** — that phase's candidate decoded
  **+0.18% MORE** items in the same scene windows, which makes the −11.78% a lower bound.
- ⚠️ **Verify BOTH binaries against their own `/MAP`.** The first control there used plain `inline`
  for its rollback, MSVC declined it, and the "control" kept only 4 of the original 9 XMM spills —
  an intermediate shape that would have measured the phase short (`17d` §4).

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

### 4.8 ⭐⭐⭐ AN **EXACT** LEVER CAN LEGITIMATELY MEASURE NULL — SO A NULL NEEDS A DIVERGENCE COUNTER

A lever proven bit-exact against the shipped path can still gate at zero for a reason that has
nothing to do with its speed: **it never diverged from the path it replaced**, because its admission
predicate selected nothing, or selected only items the old path already handled identically.

"Exact" and "ran" are independent properties, and a frame gate cannot distinguish
*fast-but-below-resolution* from *never-taken*.

- **Every null A/B must be accompanied by a counter of how many times the candidate arm took the
  NEW path**, printed per arm. Without it the null is uninterpretable.
- This is the A/B twin of §6 (an oracle that reports clean because it never ran): the same missing
  denominator, one instrument over.

### 4.9 ⭐⭐⭐ A PARITY GATE NEEDS A SAME-BINARY CONTROL — READ THE **PAIRED EXCESS**

Comparing two arms' outputs (pixels, audio, any capture) is only meaningful against **how much two
runs of ONE UNMODIFIED binary differ**. On a real game that is routinely far from zero:

| source of run-to-run divergence | measured |
|---|---|
| a scripted cutscene, same binary twice | agreed on 116/160 and 111/160 captured frames |
| a route whose **AI is frame-rate driven** (monsters, physics ticks) | same-binary control agreed on only **96%** — any speed change moves the content |
| a pure SPEED perturbation with no code change | agreed on 12/160 |

**Run four arms — ON, ON′, OFF, OFF′ — and read the excess of the lever pair over the two
same-binary control pairs.** A lever pair that lands *inside* the controls on every statistic is a
pass. ⭐ On a frame-rate-driven route a speed-changing lever should diverge **more** than a
same-speed control, so a lever pair that lands *better* than the controls is a strong pass.

⛔ Consequences worth stating explicitly:
- **The identical count alone is never the verdict.** Quote the paired excess.
- **The floor cannot be reduced by capturing more frames** — it is content divergence, not sampling
  noise. A defect confined to a few frames can hide inside it, so a **stage-level same-run oracle is
  the primary evidence** and the capture comparison is the composition check.
- Check whether the lever pair's largest differing keys **also appear in a control** with the same
  statistics. If they do, they are that arm's own run-to-run divergence, not your lever.

### 4.10 ⭐⭐ A SAME-RUN ORACLE BEATS AN UNREPEATABLE ROUTE — BUT CHECK ITS ARMING SCOPE

When a route cannot be replayed deterministically (AI, timing-dependent content, a boss fight), do
not try to fix the route. **Move the comparison inside one process:** run the candidate, restore
state, run the authority on the *same* inputs, compare in-process. That is immune to everything a
cross-run comparison suffers from.

⚠️ Two conditions, both of which have failed in practice:
1. **The authority must perform the observable effect** (the store, the commit), and the candidate's
   result is compared against what the authority actually wrote — otherwise a candidate defect can
   author output while the oracle is on.
2. **The oracle's reference must reproduce the LEVER's admission rule, not the shipped hook's.** A
   reference stepper that copied a conjunct existing only for an unrelated reason aborted with a
   confident whole-state mismatch on a slot that was never the lever's — a defect entirely in the
   reference, reported with total authority.

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

