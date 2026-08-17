# Appendix: Dark Cloud 2 — ATTRIBUTION RECIPES (how to find where a cost actually lives)

> **PROJECT-SPECIFIC, LOOKUP ONLY. One recipe per question — read the row you need, never
> top-to-bottom.** Split out of `appendix-dc2-test-routes.md` (2026-08-17, G617) because these are
> not routes: a route says *what to run*, these say *how to attribute what you measured*. The
> `§1.3x` section IDs are unchanged so every existing cross-reference still resolves.
>
> ⛔⛔ **READ THEM IN DEPENDENCY ORDER, NOT FILE ORDER.** Several were written to correct the one
> before them and say so in their titles:
>
> | order | recipe | question it answers |
> |---|---|---|
> | 1 | **§1.3j** (G593) | is a WHOLE-LAYER bypass worth building at all? — **do this FIRST** |
> | 2 | **§1.3h** (G592) | how much of the GS worker is BLOCKED? — **before §1.3f** |
> | 3 | **§1.3f** (G591) | what does the GS worker itself spend time on? |
> | 4 | **§1.3l** (G595) | a layer is priced — split it before believing its proposed fix |
> | 5 | **§1.3k** (G594) | how do I grade a lever SMALLER than the route's noise? |
> | 6 | §1.3d (G589) · §1.3e (G590) · §1.3g (G591) · §1.3i (G592) · §1.3c (G583) | narrower cases |
>
> Companions: `appendix-dc2-test-routes.md` (which route to drive) ·
> `appendix-dc2-capture-and-gates.md` (how to capture and gate the result) ·
> `17e-perf-measurement-traps.md` + `17f-ab-gate-and-oracle-traps.md` (the game-agnostic laws).

---

### §1.3d ⭐⭐⭐ How to attribute the GPU-VU COMMAND/AUTHORITY architecture (added G589)

Four instruments answer "where does the GS worker's cost actually live", and three of them had never
been run on a heavy route before G589. Run them on ONE window and de-cumulate with
`tools/g589_window.py`-style bracketing (all four print whole-run cumulative totals on a cadence
unrelated to frames, so a window figure needs the nearest preceding `[G154:perf] n=` marker at each
end).

```powershell
# 1. flush count, drain causes, and whether batches can be coalesced at all
tools\g525_route.ps1 -Route s05 -Tag edge -Set @('DC2_G523_MERGE=1','DC2_G260_STAT=1','DC2_G146_PERF=1')
# 2. how much of each per-flush pass is FIXED (recoverable only by fewer flushes)
tools\g525_route.ps1 -Route s05 -Tag fit  -Set @('DC2_G523_FIT=1')
# 3. is the backend WAITING or WORKING?
tools\g525_route.ps1 -Route s05 -Tag q    -Set @('DC2_G299_PROFILE=1')
# 4. what the backend work IS: tex/fbo/up/z/state/draw/drain/rb per target and class
tools\g525_route.ps1 -Route s05 -Tag st   -Set @('DC2_G415_CENSUS=1','DC2_G178_STAT=1')
# 5. can flush-invariant preparation be paid once per DRAIN instead of once per batch?
tools\g525_route.ps1 -Route s05 -Tag hr   -Set @('DC2_G589_CENSUS=1','DC2_G146_PERF=1')
```

The laws G589 established from them:

- ⭐⭐⭐ **`closes == batches executed`, so the FLUSH COUNT IS THE TARGET-SWITCH COUNT.** A drain does
  not create flushes; it decides when existing ones run. Any lever that reduces DRAINS
  (`DC2_G429_NO_UPLOAD_EDGE` and friends) moves *when*, never *how much*.
- ⭐⭐⭐ **`[G523:merge] mergeable=0.00%` on both `s05` and `ridepod`.** Batch coalescing is refuted.
- ⭐⭐ **`[G299:backend] q=` is the handoff and it is 5-7 us.** If someone proposes overlapping or
  batching submissions, read `q` first - G447's two-sided spin already removed the latency.
- ⭐⭐⭐ **A `submit` intercept fitted against DRAWS is not dispatch overhead.** `[G523:fit]` books
  `up`/`z`/`tex`/`rb` as "fixed" because they scale with SURFACE SIZE, not with draw count. Cross-
  check every fit intercept against `[G415:stage]` before calling it removable.
- ⭐⭐⭐ **`[G589:drain] HEADROOM` is a property of the WINDOW.** 22.13% on `s05 n=1021..2461`,
  **80.17%** on the Ridepod boss (bracket `n=1291..1831`), 27.8% on the diluted `ridepod` whole run.
  Never conclude from one window, and never from a whole-run cumulative.
- ⛔ **`[G589:memo] badResult=0 badKey=0` with `checks=0` is a FAILED run.** Read `checks=`/`hits=`
  first. G589's slice-1 memo printed a clean-looking oracle line having compared nothing.

Full evidence: `plans/phase-G589-fix-log.md`.

### §1.3e ⭐⭐⭐ How to attribute SURFACE AUTHORITY (added G590)

"Where does the GPU copy keep having to be re-established, and is it already correct?" Three
instruments, all default-off. Run them on ONE window and de-cumulate with `tools/g590_window.py`
(all print whole-run cumulative totals on an event cadence unrelated to frames).

```powershell
# 1. the ledger: colour/depth upload decisions + materializations + readbacks, per TARGET x REASON
tools\g525_route.ps1 -Route s05 -Tag auth -Set @('DC2_G590_CENSUS=1','DC2_G146_PERF=1')
# 2. the ENTRY CONDITION: is the GPU copy already equal to what the upload will write?
#    ⚠️ PERTURBING - blocking readback per upload. NEVER in a timing arm.
tools\g525_route.ps1 -Route s05 -Tag sh   -Set @('DC2_G590_CENSUS=1','DC2_G590_SHADOW=1')
# 3. WHY the depth upload fires, and which conjunct blocks a proposed gate
tools\g525_route.ps1 -Route s05 -Tag zwhy -Set @('DC2_G590_ZWHY=1','DC2_G146_PERF=1')
python tools\g590_window.py captures\g477_sh_err.txt 1021 2461
```

The laws G590 established from them:

- ⛔ **A SHARE OF THE BACKEND IS NOT A PRICE.** `[G415:stage]` de-cumulated over `s05 n=1021..2461`
  puts the whole GPU backend at **3.265 ms/frame** of a 36.20 ms/f window. So "~40% of backend work
  is upload" is **0.95 ms/frame**, and the whole surface-transfer family (adding the front-end `fbz`
  pack) is **~5 ms/f = ~14%** - the largest coherent family, **not the pole**.
- ⭐⭐⭐ **QUOTE `rowEq%`, NOT PIXEL `eq%`.** `0x0`/`0x68` colour uploads read **99.13% pixel-equal
  and 0.00-0.03% ROW-equal**. A row-granular skip on them buys exactly nothing; ranking by pixel
  eq% sends a phase after the wrong target.
- ⭐⭐⭐ **A SHADOW READ THAT *FAILS* IS A FINDING.** `0x139.upSnapInv` - the largest single
  colour-upload line in `[G415]` (1111 ms of the whole run) - fails its shadow read **100% of the
  time** because no FBO exists. That is non-resident repopulation: the answer is **RESIDENCY, not an
  upload skip**, and G304's local minimum applies.
- ⭐⭐⭐ **THE DEPTH VERDICT IS PER-ROUTE.** Windowed depth upload: **eq=100.00% over 2.82 billion
  values on `s05`**, but **90.8-96.7% on the Ridepod boss**. Never promote a depth-authority lever
  off `s05` alone; validate on `ridepod`, where it is genuinely live and a missed writer will show.
- ⭐⭐⭐ **`[G590:zwhy] blocked:` NAMES A STRUCTURALLY DEAD GATE.** `privMir=19968/19968` on `s05` and
  `35328/35328` on `ridepod`: 100% of windowed depth uploads run under the G411 private mirror, for
  which `g_g274LastZGen` is deliberately erased - so `[G590:auth] zSkipRes = 0` on every target and
  G274's default-ON resident-depth elision **never fires**. Take the census BEFORE the gate and
  count every conjunct, or a lever that cannot fire looks identical to a lever that found nothing.
- ⭐⭐ **A SURFACE HELD OUTSIDE ALIASED VRAM HAS NO PAGE GENERATIONS, HENCE NO AUTHORITY RECORD.**
  The private depth mirror is a plain `std::vector<uint32_t>` (`g403DisplayZBuf`). Check for that
  class before trusting any residency census that keys on `g_g178PageGen`.

Full evidence: `plans/phase-G590-fix-log.md`.

### §1.3l ⭐⭐⭐ How to attack a priced LAYER (added G595) — split it before believing its proposed fix

```powershell
# 1. the layer's own clock. ⚠️ read it with g595_draw.py, NOT by eye: the raw laps carry ~15 ns/call
#    of steady_clock, which is 35% of Layer 2's apparent size.
tools\g525_route.ps1 -Route s05 -Tag cen -Set @('DC2_G526_DRAW=1','DC2_G526_SPLIT=1','DC2_G146_PERF=1')
python tools\g595_draw.py captures\g477_cen_err.txt 1021 2461
# 2. if the prologue (`pro`) leads, split it too
tools\g525_route.ps1 -Route s05 -Tag pro -Set @('DC2_G595_PRO=1','DC2_G526_DRAW=1','DC2_G526_SPLIT=1')
python tools\g595_pro.py captures\g477_pro_err.txt 1021 2461
# 3. exactness on BOTH GS-poled routes, then gate the frame on ridepod (s05 cannot resolve it)
tools\g525_route.ps1 -Route s05     -Tag vfy -Set @('DC2_G595_NOTEVFY=1')
tools\g525_route.ps1 -Route ridepod -Tag vfy -Set @('DC2_G595_NOTEVFY=1')
tools\g525_route.ps1 -Route ridepod -Tag ab  -Ab g595both
```

- ⭐⭐⭐ **SPLIT THE LAYER BEFORE BELIEVING ITS ONE PROPOSED FIX.** The roadmap carried Layer 2 as a
  single 5.44 ms/f number whose only named attack was a ~700-site `G144Entry` refactor. The split
  named **two levers worth 31% of it, neither touching the struct** — and the refactor was already
  ⛔ NO-GO in the project's own table (G521 §4's pad probe, ≤0.115 ms/f).
- ⭐⭐⭐ **"THIS LEVER BUYS NOTHING" IS SCOPED TO THE ROUTE THAT MEASURED IT.** G270's line deferral
  sat default-off for **21 phases** on a MAP-0 verdict. On `s05` it holds **18.3% of the layer**, and
  sizing it cost one census run and **no build** — the lever already existed, exact and gated.
  Check `[G526:type] inline(ls)` before assuming a route has the population.
- ⭐⭐⭐ **A NON-DEFERRABLE PRIMITIVE'S COST IS THE BARRIER IT FORCES.** `[G526:split]` splits the
  inline exit into `drain` 51.8 µs + `edge` 175.4 µs + `raster` **15 µs**. The raster is 6%. The fix
  is admission, never a faster inner loop.
- ⭐⭐ **MEMOIZE AGAINST THE ACCUMULATOR, NOT THE COLLECTION.** Keying the page-range memo on a `seq`
  bumped by `G260RangeSet::reset()` — the accumulators' only shrink, in one place — made it immune
  to all **eleven** `g_g144List` clear/swap sites without plumbing a single one.
- ⭐⭐ **GATE A GS LEVER ON `ridepod`.** The same promotion read blocked **−0.152 ± 0.217 (null)** on
  `s05` and **−0.436 at t = −2.46, 51/75 windows** on `ridepod`. Route choice is part of the instrument.
- ⛔ **TWO REFUTED GUESSES AT A CODEGEN REGRESSION IS THE SIGNAL TO STOP.** A +0.14 ms/f prologue
  residual survived both a cached-bool hoist of the arm accessor and a conjunct reorder. Report it
  inside the net; hand it to `/MAP`+capstone, not a third source-level guess.

Full evidence: `plans/phase-G595-fix-log.md`.

### §1.3k ⭐⭐⭐ How to grade a lever SMALLER than the route's noise (added G594) — read before designing one

```powershell
# 1. price the leaf's OWN calls per frame FIRST — never `cost/flush x flushes/frame`
tools\g525_route.ps1 -Route s05 -Tag cen -Set @('DC2_G594_CENSUS=1','DC2_G146_PERF=1')
# 2. the payoff: TWO census runs, levers off then on, compared on the thread-local sub-timer
tools\g525_route.ps1 -Route s05 -Tag t0 -Set @('DC2_G594_CENSUS=1')
tools\g525_route.ps1 -Route s05 -Tag t1 -Set @('DC2_G594_CENSUS=1','DC2_G594_FASTHASH=1','DC2_G594_FASTDESWZ=1')
python tools\g594_leaf.py captures\g477_t0_err.txt captures\g477_t1_err.txt
# 3. exactness, with the comparison COUNT printed beside every bad=
tools\g525_route.ps1 -Route s05 -Tag vfy -Set @('DC2_G594_CENSUS=1','DC2_G594_HASHVFY=1','DC2_G594_DESWVFY=1','DC2_G594_WALKVFY=1')
```

- ⭐⭐⭐ **A PER-CALL COST TIMES THE FLUSH COUNT IS NOT A PER-FRAME COST.** `[G323:census]
  logical=0.043 ms/flush × 56.8` ranked `g310EnsureCurrent` at 2.44 ms/f. It runs **14.22×/frame**
  and costs **1.257**. Count the leaf's own calls before ranking it — that one substitution was a
  factor of 64 on the sub-item inside it.
- ⭐⭐⭐ **`s05`'s WHOLE-ROUTE PAIRED A/B RESOLVES ~1 ms/f AT BEST** (blocked `se` 0.236 ms/f), and it
  **dilutes a GS lever with the route's own light windows**, where the frame sits on the 60 Hz cap
  and a GS saving converts at ~0 (§1.3j / G593 §0.1c). A 0.62 ms/f lever measured `blocked mean
  −0.029, t=−0.12, fav=37/65` — **null, not negative**. Build the sub-timer in the SAME edit as the
  lever or the slice cannot be graded at all.
- ⚠️ When the RAW and BLOCKED estimators **disagree in sign** on this route (here +5.06% vs −0.03),
  the raw one is comparing two unequal SCENE MIXES on a ramping route. Read the blocked one.
- ⭐⭐ **DERIVE A SWIZZLE TABLE BY PROBING THE SHIPPED READER**, never by hand-deriving the layout:
  fill a private page with its own word indices and ask `GSMem::ReadCT32` where each texel lives.
  The plan then agrees with shipped code by construction and fails closed if it misses a texel.
  (`[G594:plan] avgRun=2.00` — PSMCT32 interleaves at 2 texels, so the win was 1.8×, not 10×.)
- ⭐⭐ **AN INTERNAL EQUALITY TOKEN IS FREE TO CHANGE.** A hash produced and consumed only inside one
  unit in one process, never a rendering input, can be swapped for any faster function — but the
  oracle must compare the **decision** ("did this page change?"), not the bits. 13.3× here.
- ⛔ **`g310ApplyDesired` NEVER RUNS** (`applyNs=0.000`, `uninit/calls=1.0000`): every atlas refresh
  is a full 512×512 composite because `g310NoteProducerWrite`/`g310NoteTargetClean` de-initialize
  unless `g311On()`, and G311 carries an unflipped bring-up default. ⛔ **And G311 stays NO-GO on
  `s05` too**: composite 25.2 µs (vs 230.8 on MAP-0) and 97–127 of 128 pages change in **100%** of
  composites (`DC2_G311_CENSUS=1` → `[G311:stat]`, `[G311:delta]`).

Full evidence: `plans/phase-G594-fix-log.md`.

### §1.3j ⭐⭐⭐ How to price a WHOLE-LAYER bypass before building it (added G593) — do this FIRST

```powershell
# the ceiling: 4 runs, order-balanced A B B A, invariant on BOTH arms
tools\g525_route.ps1 -Route s05 -Tag c1 -Set @('DC2_G434_INV=1')
tools\g525_route.ps1 -Route s05 -Tag n1 -Set @('DC2_G434_INV=1','DC2_G434_NO_DRAW=1')
#   ... n2, c2 ...
# the layer split: the SAME instrument set on default / NO_DRAW / NO_DRAW+NOREG
$i = @('DC2_G434_INV=1','DC2_G147_PERF=1','DC2_G146_PERF=1','DC2_G332_CENSUS=1','DC2_G303_INSTR=1')
tools\g525_route.ps1 -Route s05 -Tag p0 -Set $i
tools\g525_route.ps1 -Route s05 -Tag p1 -Set ($i + 'DC2_G434_NO_DRAW=1')
tools\g525_route.ps1 -Route s05 -Tag p2 -Set ($i + 'DC2_G434_NO_DRAW=1' + 'DC2_G593_NOREG=1')
python tools\g593_window.py captures\g477_p0_err.txt ... --n0 1021 --n1 2461 --tag G434:inv --tag G147:gif
```

- ⭐⭐⭐ **`frame ≈ max(VU1 busy, GS-worker own)`.** A GS lever's budget is the HEADROOM
  `GS own − VU1 busy` (10.96 ms/f on `s05`, 9.75 on `ridepod`) at **≈1:1** — never a fraction of it.
  The tell that the model is right is `[G303:vu1w] gsStallMs/f` flipping **0.000 → 14.420** between
  the arms: in the default arm the GS worker never waits for VU1 because it IS the pole.
- ⭐⭐⭐ **A CEILING is scoped to the measuring route's pole.** G434 ran this same probe on lean
  MAP-0 and got −2.83 ms/f; on `s05` it reads **−10.098 (−27.9%)** and on `ridepod` **−12.19
  (−32.3%)**. Re-run a ceiling before inheriting its verdict.
- ⭐⭐⭐ **Never divide the ceiling by the wall it deleted.** The probe overshoots past the VU1
  floor, so that quotient (0.47 here) understates the real conversion by half.
- ⭐⭐ **A ceiling probe is also the cleanest ATTRIBUTION instrument you own**: with the downstream
  deleted, every per-layer timer reads its layer with nothing able to react. `NO_DRAW`'s residual
  `image` = 1.092 ms/f independently confirmed `[G313:seg] writer` = 1.112.
- ⚠️ **A second ceiling probe stacked on the first measures NOTHING in frame terms** —
  `NO_DRAW+NOREG` is −0.19 ms/f of frame for 2.08 ms/f of deleted GS CPU. Read a thread-local
  sub-timer (`[G147:gif] parserOther`), never `avgFrameMs`.
- ⚠️ **Quote the workload invariant.** `[G434:inv] kicks/f` agreed to 0.002%–0.12% across every arm;
  without it "−27.9%" is a difference between two workloads, not a measurement.

### §1.3h ⭐⭐⭐ How to attribute the GS worker's BLOCKED half (added G592) — do this BEFORE §1.3f

A PC sampler names the wait PRIMITIVE; it cannot price the wait. Put a wall clock on every edge at
which the pole thread can block, and COUNT them:

```powershell
tools\g525_route.ps1 -Route s05 -Tag edge -Set @('DC2_G447_EDGE=1','DC2_G146_PERF=1')
# each [G447:edge] line is already a 30-frame DELTA — bracket by the nearest [G154:perf] n= markers
# and average the lines inside the window; do NOT de-cumulate it.
```

- ⭐⭐⭐ **On `s05 n=1021..2461` the GS worker is 31% BLOCKED (11.67 ms/f) and 69% its OWN CPU
  (26.09 ms/f)** of a 37.76 ms/f window. G591's "~81% waiting" was a PC-sample artefact: under
  `DC2_G563_RAWPC_ONLY=1` every non-exe address prints `pref=0x0` **with no module name**, so
  `ucrtbase` `memcpy` (this backend's 512×512 surface pack) sits in the same bucket as an `ntdll`
  wait. The sampler's OWN cross-tab agrees with the clock once weighted by stage share (32.3%).
- ⭐⭐⭐ **Rank the blocked half by ms/f, not by item count.** `backend-readcolor` is **5.605 ms/f
  from 8.3 items** (673 µs each) while `backend-render` is 4.125 from **54.6**. The expensive edge
  is the rare one.
- ⛔ **Both big edges are already closed.** `readcolor` by G495 (drain 75% / transfer 25%, and the
  drain is real GPU catch-up: an empty queue ahead drains 0.33 ms, 410 draws ahead drain 1.65 ms;
  per-surface redundancy 0.0%; no earlier issue point). `render` by G493 (removing the wait from 47%
  of round trips made the frame WORSE 4/4 — **the GS front thread and the GL worker do not compose
  on this host; remove GL work, do not schedule it**).
- ⭐⭐ **Therefore a "reduce the submission count" redesign is mis-aimed.** The 56.8 submissions/frame
  total 11.67 ms/f of wait. The 26.09 ms/f of GS-worker CPU is the quantity a VU1 → XGKICK/GIF → GS
  fast path is actually worth building against.

### §1.3i ⭐⭐ How to gate a PUBLICATION that has no VRAM range (added G592)

```powershell
tools\g525_route.ps1 -Route s05     -Tag c  -Set @('DC2_G592_CENSUS=1','DC2_G146_PERF=1')
tools\g525_route.ps1 -Route ridepod -Tag g  -Set @('DC2_G592_CENSUS=1','DC2_G411_STAT=1')
tools\g525_route.ps1 -Route s05     -Ab pubcons -Tag ab
```

- ⭐⭐⭐ **"This surface has no range to scope against" is an argument about RANGES, not CONSUMERS.**
  The private mirror has no guest-VRAM range, but the batch about to be replayed either contains a
  draw that can reach it or it does not — and that is computable from the captured `G144Entry.ctx`
  in the single-threaded flush prologue.
- ⭐⭐ **The oracle is an INVARIANT, not "the old path found no work"** (g528): no CPU draw may take
  the `g403DisplayZ` path while a private-mirror owner is pending. `escapedReplay` MUST read 0.
  Hook it at the PREDICATE and **not** at `&& zWrite` — a depth TEST reads the mirror too.
- ⚠️ **A skip that leaves an owner PENDING inflates every per-prologue counter in the candidate
  arm** (7,168 → 22,528 on `s05`), because the same owner is re-counted until something publishes
  it. Count the PUBLICATIONS (`[G411:depth] publishes`), not the decisions.
- ⚠️ **Ship the per-draw invariant probe DEFAULT-OFF even when the lever ships default-ON.** It is
  arm-independent, so the A/B cannot see it, and it would ride one relaxed atomic per draw into the
  shipped binary.

### §1.3f ⭐⭐⭐ How to attribute the GS WORKER ITSELF (added G591) — ⚠️ read §1.3h first

Every attribution before G591 decomposed the *inside* of the flush and therefore could only
apportion whatever fraction of the worker is CPU. Point the sampler at the thread first:

```powershell
tools\g525_route.ps1 -Route s05 -Tag pc -Set @('DC2_G446_GSPROF=1','DC2_G563_RAWPC_ONLY=1','DC2_G146_PERF=1')
python tools\g504_mapres.py build64\Release\dc2_runner.map captures\g477_pc_err.txt
```

- ⛔ **REFUTED BY G592 (§1.3h): "on `s05` the GS worker is ~81% WAITING" is wrong.** The 54.17%
  "outside the exe image" summed `pref=0x0` rows, which carry **no module name**, so CRT `memcpy`
  was counted as wait. A direct wall clock reads **31% blocked**, and this sampler's own
  `ofWhichKernelWait` cross-tab weighted by stage share reads **32.3%**. Use the distribution of
  our OWN rows (largest leaf `g418UnpackColorRows` 2.81%); never sum the anonymous bucket.
- ⭐⭐ **Read `[G446:stage]` beside it or the share means nothing**: `window-popped 94.07%`
  (`ofWhichKernelWait=30.85%`) against `idle-at-queue 3.05%` — the worker HAS work and is blocked on
  the GPU, which is a different finding from "the worker is idle".
- ⛔ **Subtract the measured 31% (§1.3h) before quoting any "X% of the GS worker is Y."**
  `[G523:fit]`, `[G415:stage]` and `[G299]` all apportion the CPU 69%.
- ⚠️ The sampler perturbs absolute frame time (a suspend/resume per sample). Distribution only.

### §1.3g ⭐⭐ How to attribute a surface that has NO page generations (added G591)

The G411 private depth mirror lives outside aliased VRAM, so `g_g178PageGen` cannot see it. The
pattern that gives such a surface an ownership record:

```powershell
tools\g525_route.ps1 -Route s05     -Tag c   -Set @('DC2_G591_CENSUS=1','DC2_G146_PERF=1')
tools\g525_route.ps1 -Route ridepod -Tag zv  -Set @('DC2_G591_CENSUS=1','DC2_G591_ZVERIFY=1','DC2_G411_STAT=1')
tools\g525_route.ps1 -Route ridepod -Ab privz -Tag ab
```

- ⭐⭐⭐ **Bump the epoch at the per-draw PREDICATE, never at the write sites.** `g403DisplayZ &&
  zWrite` is the gate every `g403DisplayZWrite` already sits behind, so a future writer inside a CPU
  draw is covered for free — that is "observe the input", not "enumerate the writers".
- ⭐⭐ **`[G591:zwhy] mirror(draws= bandBumps=)` is the discriminator.** It is FROZEN through `s05`'s
  entire cutscene (no CPU draw touches the mirror) and 22 M on `ridepod`. A lever whose census reads
  100% clean on one route and 30% on another is working, not broken.
- ⭐⭐ **Validate on `ridepod`, not `s05`.** G590 measured the windowed depth upload at 100.00% equal
  on `s05` but only 90.8–96.7% on `ridepod`, so `ridepod` is the only route where a missed writer
  can show. G591's oracle compared 1.64 G values there with `bad=0`.
- ⛔ **The paired A/B's RAW delta is not the result on `s05`.** The instrument gives the control 4–9%
  more frames in every run, and `s05` ranges 18→36 ms/f, so the unpaired split reads +5% while the
  paired `blocked(mean= t= fav=)` estimator reads null. `ridepod` is uniformly ~31 ms/f and the two
  estimators agree there.

### §1.3c ⭐ How to attribute a Group-A window (added G583)

The Group-A cost is **the CPU band replay the upload edge schedules**, not the upload edge. Two
tools, both localized to one `n=` window — the whole-run tail mixes menus/cutscene/boss/workshop
and ranks a different target:

```powershell
tools\g525_route.ps1 -Route ridepod -Tag t -Set @('DC2_G290_PROBE=1','DC2_G260_STAT=1','DC2_G146_PERF=1')
python tools\g583_drain_window.py captures\g477_t_err.txt 1441 1861    # splits the drain
tools\g525_route.ps1 -Route ridepod -Tag c -Set @('DC2_G583_REJCENSUS=1')
python tools\g583_rejshape.py     captures\g477_c_err.txt 1441 1861    # ranks the fallbacks
```

- ⭐⭐ **Rank the fallback census by REPLAYED ENTRIES, never by events.** entries/event spans
  132..1,735 across the boss window, so an event ranking inverts the order.
- ⭐⭐ **`[G262:census]` and `[G265:census]` cannot see this population.** The first is gated on the
  five named G248 targets, the second on fbp 0/0x68. fbp=0x13b — the boss window's largest
  fallback source — is in neither.
- ⛔ **`[G290:gpufail]` prints ENTRY 0**, not the entry that rejected (the classifier returns on
  the first failure). Use `DC2_G583_REJCENSUS=1`, which stashes the rejecter.

⭐ **A same-run oracle CAN gate this route, even though a cross-arm pixel A/B cannot.**
§1.2b's warning (and G573's) that `ridepod`'s boss defeats a pixel comparison is about comparing
two RUNS — the boss AI does not replay. A same-run oracle (`DC2_G583_VERIFY_TRN13B=1`: render the
batch on the GPU, restore, replay on the CPU, compare in-process) is immune to that, and G583
measured two 420 s runs as **batch-for-batch identical** because the CPU replay stays
authoritative under verification. Use that shape for any 0x13b/0x13d/0x139 authority change.

