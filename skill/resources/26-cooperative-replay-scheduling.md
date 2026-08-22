# Cooperative Work Scheduling for a Rasteriser Fork/Join

> **GENERIC.** How to diagnose and repair a parallel dispatch inside a hot renderer loop: naming the
> binding constraint before choosing a lever, the exactness contract that makes a re-partition free,
> the scheduler that subsumes a work estimator, and the two ways a pixel gate lies about it.
> Worked instance: DC2 G637 (`plans/phase-G637-fix-log.md`).

---

## 1. The defect class: a fixed partition inside a barrier

The canonical shape is a persistent worker pool that splits a range into **as many equal pieces as
it has workers**, hands piece 0 to the caller, and blocks the caller until the rest report:

```cpp
run(y0, y1, lanes, job)   // lanes EQUAL row bands; caller runs band 0 then waits on a countdown
```

This is correct, cheap to write, and structurally wrong for two independent reasons:

1. **Equal pieces are not equal work.** The join is paced by the single worst piece while every
   other worker idles. The loss scales with how uneven the content is, and rendering content is
   *always* uneven — geometry clusters.
2. **The caller is the thread you can least afford to idle.** In a renderer the dispatching thread
   is usually the one on the critical path. After finishing its own piece it contributes nothing.

⭐ **Neither is fixed by changing the piece COUNT**, which is the lever everyone reaches for first.
More pieces than workers helps only if pieces are claimed *dynamically*; with a static assignment,
`n` pieces to `n` workers just relabels the same imbalance.

---

## 2. ⭐⭐⭐ FIRST: split the wall into `sum` and `max`, or you will fix the wrong thing

Before designing anything, instrument each dispatch with three numbers:

| number | meaning |
|---|---|
| `wall` | what the caller observed (fork → join) |
| `sum` | the aggregate time of every participant's pieces |
| `max` | the single worst piece |

Then read it:

| reading | binding constraint | the lever that can help |
|---|---|---|
| `max ≈ wall` | **tail-bound** — one piece paces the join | scheduling: over-decompose + steal |
| `sum/workers > max` | **aggregate-bound** — there is simply that much work | only DELETING work; no scheduler change helps |
| `wall >> max` and `wall >> sum/workers` | **overhead-bound** — wake latency, join convoy, contention | the dispatch machinery itself |

⛔ **Skipping this step is how a phase spends itself on a scheduler for an aggregate-bound stage**
(or vice versa). In DC2 G637 the dispatch was tail-bound *before* the lever (`max ≈ wall`, caller's
band 5.3× cheaper than the mean worker band) and became **aggregate-bound after** — which is
precisely why the tuning sweep then stopped rewarding finer chunks, and why the next lever on that
stage has to be a kernel lever, not a scheduling one.

⛔ **The `sum` of one instrument is not comparable to the `sum` of another.** If the pre-lever census
adds per-item atomics across all workers and the post-lever one does not, the pre-lever aggregate
carries contention the post-lever aggregate cannot have. Compare `wall` (both instruments measure it
the same way) or re-measure both arms under one instrument.

---

## 3. The exactness contract that makes re-partitioning free

A parallel replay of an ordered command stream over a spatial range is exact when:

1. pieces are **contiguous** in the split dimension,
2. pieces are **disjoint** and **cover the range exactly once**,
3. each piece replays the **whole** command list **in submission order**, clipped to its own range.

(1)+(2) give disjoint destination writes; (3) preserves per-pixel blend/depth order.

⭐ **Nothing in that contract mentions HOW the range was cut or WHICH thread ran a piece.** So any
re-partition and any scheduling policy that preserves those three properties is exact *by
construction* — you inherit the original design's proof rather than needing a new one. Say so
explicitly in the code, and back it with a cheap partition oracle (fences monotone, first == lo,
last == hi+1) rather than a per-pixel comparison, because the per-pixel arithmetic did not change.

⚠️ **The one real hazard**: a kernel that READS outside its own piece (e.g. sampling a texture that
is also the render target of this same batch). Such a kernel was already broken under the original
partition — but moving the fences moves the artefact, so it will surface as a regression in *your*
change. Enumerate the readers before assuming.

---

## 4. The scheduler: over-decompose, claim dynamically, let the caller participate

```
chunks   = workers × OVER            (OVER ≈ 4; clamp by a minimum piece size)
cursor   = ONE atomic packing generation<<32 | next index
claim()  = CAS loop; fails if the generation moved on  → a late worker cannot steal from the
                                                          NEXT dispatch
remaining= atomic countdown of chunks not yet COMPLETED (not "handed out")
caller   = claims and runs chunks in the same loop as the workers, THEN waits on `remaining`
seats    = bound on how many workers may join, so a pool larger than the requested concurrency
           does not widen it
```

Why each piece matters:

- **Generation-tagged cursor.** A mutex re-check per claim (the usual way to stop a late worker
  taking the next generation's work) is fine at 8 claims per dispatch and is a serialisation point at
  64. Packing the generation into the cursor makes the claim lock-free and the safety property
  automatic.
- **Countdown on COMPLETION, not on hand-out.** This is what guarantees no job body is live when the
  dispatch returns, which in turn is what lets the caller's fence array and closure be stack-scoped.
- **The caller in the claim loop.** This is usually the single biggest term, because the caller is
  the critical-path thread. It also removes the "caller gets exactly piece 0" bias, which is what
  made piece 0 systematically the cheapest.

---

## 5. ⛔ Do NOT also build a work estimator — the scheduler subsumes it, then it costs you

The intuitive companion lever is a **weight-balanced partition**: estimate per-row work (a
difference array over item bounding boxes is O(items + rows) and cheap), then place fences at equal
cumulative weight.

**Measure it before you believe it.** In DC2 G637, at the same chunk count, weight-balanced fences
were worse than plain equal rows on **both** axes:

| fences | wall / dispatch | aggregate `sum` | worst chunk |
|---|---:|---:|---:|
| weight-balanced | 1.011 ms | 7.119 ms | 0.351 |
| **equal rows** | **0.943 ms** | **6.239 ms** | 0.547 |

⭐⭐⭐ **Two compounding reasons, both general:**

1. **The scheduler already balances.** Dynamic claiming over 4× more chunks absorbs estimator error
   *and* estimator absence. The better `max` the estimator buys is a number nobody was waiting for
   once the dispatch is aggregate-bound.
2. **A weight-balanced fence set is actively harmful to AGGREGATE work.** It makes chunks *short*
   exactly where the items are dense — so more items straddle a fence, and each straddling item
   repeats its per-item front-end in one more chunk. Equal fences spread the boundaries uniformly
   and therefore cross fewer items.

⭐ **The general law: `aggregate ≈ base + k × (number of item/chunk crossings)`.** Any partition
policy has to be judged on crossings, not just on balance. In G637, `sum` rose monotonically from
6.148 to 8.342 ms as chunks went 8 → 60 (**+42 µs per extra chunk**) while `max` fell 0.974 → 0.235.
Wall is the sum of those opposing terms and is **flat-bottomed** over a wide range — so pick the
middle of the flat region and stop tuning.

---

## 6. ⛔ And do NOT assume an index beats a linear scan

The other intuitive companion is a **compiled chunk→item index** (counting-sort the items into
per-chunk buckets once, so a chunk visits only the items that reach it, instead of testing all of
them). It replaces `chunks × items` predicate evaluations with one slot per intersection.

**It measured NULL.** Order-balanced A B B A, the two blocks disagreed in sign. Why the ceiling was
smaller than the arithmetic suggested:

- The scan it deletes is **two well-predicted compares over a contiguous array** with hardware
  prefetch. Modern cores do this at ~1 item/cycle; "N times more predicate evaluations" is not
  "N times more time".
- The index substitutes a **gather** for a stream.
- Worst of all, it **moves work ONTO the dispatching thread** — the build is serial, in front of the
  fork — to take work off `workers` parallel lanes. On a pole thread that trade is negative before
  it starts.

⭐ **Rule: a prefilter is only worth indexing when the predicate is expensive or the data is not
contiguous.** Price the scan (`part` vs `sum`) before writing the index.

---

## 7. ⭐⭐⭐ The pixel gate trap: a deterministic window is deterministic AT FIXED SPEED

A scheduling change is usually a **large speed change**, and that breaks the standard cross-arm pixel
gate in a way slower prototypes never expose:

- Capture keys are bucketed on a **guest/script clock**, but some content advances on **host
  presents** — UI cursors, particles, anything animated by the presentation loop. Two arms at
  different frame rates land on different animation sub-phases *inside the same bucket*.
- A temporal-minimum estimator over ±N capture keys cannot collapse a **sub-key** offset.
- Result: the candidate scores above a 0.00 control-vs-control floor and looks like a defect. The
  contact sheet shows a cursor sprite, a particle, or a moving silhouette — never a wrong colour,
  never missing geometry.

⭐ **THE DISCRIMINATOR: add a SPEED-PERTURBED SAME-BINARY CONTROL.** Take the unmodified renderer and
slow it with an injection probe (`DC2_G431_GS_SLOW_US` in DC2 — a busy-spin on the worker, no
renderer change at all), then score it against the ordinary control. Two readings settle it:

| observation | conclusion |
|---|---|
| the unmodified-but-slowed control reproduces the candidate's signature (same p90/max/worst key) | the signature is **speed→phase**, not pixels — the candidate is inside the envelope speed alone opens |
| the candidate **also** run at the perturbed speed collapses to the 0.00 floor against the ordinary control | the candidate is **bit-identical** to the shipped renderer; the only variable was speed |

G637 got both, and additionally the candidate's divergence (1.24) was **strictly smaller** than the
zero-modification control's (1.62). A renderer that was not modified cannot have a rendering defect,
so that bounds the candidate from above.

⭐ **A second, independent exactness reading for free**: capture the candidate at two different speeds
and score them against each other. Identical scores prove the output is invariant to **both** speed
and steal order — which is the empirical form of §3's contract, and the thing a partition oracle
cannot show.

---

## 7b. Before any of this: PROVE the dispatch is where the time is (added G638)

Every section above assumes the fork/join is a material share of the pole. Twice now that assumption
came from a census whose bracket was **wider than its name**, and the correction was large.

**7b.1 — A census's header comment is not its bracket. Read the timestamps.**
One dispatch census documented its `wall` as *"the pool call, caller-observed"*. Its clock was
actually reset before the bounding-box scan and read again after the entire dispatch body, so `wall`
also contained per-batch preparation, admission walks and a whole second GPU path. On the route
where that mattered it overstated the pool by **7×** (9,957 vs 134 µs/dispatch). Everything derived
from it inherited the error, including a headline "the caller sleeps 91% of the dispatch". **When a
number is about to select an architecture, open the instrument and check where its clock starts and
stops.**

**7b.2 — Bucket the barrier by CALL SITE, and do it without touching the call sites.**
A worker pool usually has several callers, and a lever normally covers one of them. Capture
`_ReturnAddress()` inside the pool entry point and bucket on it: no call site has to change, none can
be forgotten, and a later-added one appears automatically. Print preferred-base addresses and resolve
them offline against the linker map — symbolising in-process costs tens of seconds on a large image,
and the report usually runs on the very thread you are measuring.

**7b.3 — Rank by `wait = wall − lane`, never by `wall`.**
`wall` contains the caller's own share of the work, which is real and which no scheduling change can
delete. Only the waiting half is recoverable by a scheduler. Ranking by `wall` promotes the busiest
call site; ranking by `wait` promotes the one a scheduler can actually help. In the worked case the
two orders disagreed, and the six non-primary call sites — the ones an earlier phase had listed as
the obvious follow-on work — turned out to be **0.52 ms/f in total**.

**7b.4 — A blocked pole thread is usually blocked on the GPU backend, not on its own worker pool.**
A PC sampler said a third of the pole thread was in a kernel wait, which reads like a barrier problem.
Split by owner first: **≈9.9 ms/f was synchronous GPU submissions** (46.9 per frame at 212 µs each)
against **1.84 ms/f of barrier**. Check the queue/handoff time before the work time — when the
handoff is single-digit microseconds the backend is *working*, not being reached slowly, and the
lever is pipelining or deleting the submissions, not scheduling the pool.

---

## 8. Checklist

1. Instrument the dispatch: `wall`, `sum`, `max`, serial prologue. Name the binding constraint (§2).
2. If tail-bound: over-decompose, claim dynamically, **put the caller in the claim loop** (§4).
3. Preserve the three exactness properties and say so; add a partition oracle, not a pixel oracle (§3).
4. **Do not** add a work estimator or a bucket index without measuring each on its own (§5, §6).
5. Gate pixels against a **speed-perturbed same-binary control**, and score the candidate against
   itself at two speeds (§7).
0. **First** prove the dispatch is a material share of the pole, bucket the barrier by CALL SITE, and
   rank by `wait`, not `wall` (§7b) — twice this step overturned the phase's premise.
6. Re-read `sum`/`max` after the lever — a tail-bound stage usually becomes aggregate-bound, which
   changes what the NEXT lever must be, and re-derive the thread pole because you may have moved it.
