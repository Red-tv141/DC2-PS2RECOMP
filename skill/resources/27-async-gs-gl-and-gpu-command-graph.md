# Asynchronous GS/GL Backend Pipelines & GPU Command Graphs — what actually happens when you build one

> **GENERIC & PROJECT REUSABLE.** This file was written as a DESIGN for decoupling a GS worker from
> GPU submission latency. It has since been **built and measured end to end** (DC2 G640, after
> G305/G444/G452/G493), and the design does not pay for the reason in §2. Read §2 and §3 before
> proposing any asynchronous-submission or command-ring work; §5 is the instrument that decides it
> in one run.

---

## 1. The defect class, as it presents

The GS worker parses GIF packets and translates them to host graphics commands, then blocks on the
result:

```cpp
// The shape everyone starts from
void submitBatch(const DrawBatch &batch) {
    auto future = g_glBackend->enqueue(batch);
    future.wait();               // the front end sleeps here
}
```

The census that motivates the work always looks the same: *"the pole thread is N ms/f blocked inside
backend round trips, and the enqueue latency is only ~10 µs, so it is idly waiting for the GPU."*

⛔ **That reading is usually wrong in two ways at once, and both are checkable before you write
code.**

---

## 2. ⭐⭐⭐ THE LAW: a single-worker backend pins the pole's blocked time to its OWN serial occupancy

> **`recoverable_by_scheduling = pole_blocked − backend_serial_busy`.**
> Everything else is BACKEND WORK. No scheduling change can reach it.

Measure both terms — they come from different instruments on different threads:

| term | where it is measured | DC2 G640 value (`dragon`) |
|---|---|---|
| `pole_blocked` | a wall clock around every blocking edge, **on the caller** | 10.1–10.8 ms/f |
| `backend_serial_busy` | a wall clock around each item, **inside the worker** | 8.7–9.3 ms/f |
| **difference = the whole prize** | | **≈1.4 ms/f** |

**Why the difference is the whole prize.** The backend is one thread with one context, executing a
FIFO. If the front end must eventually consume every result, then over a frame the worker's items
are all executed, serially, and the front end cannot finish before they do. Making item *i*
non-blocking does not delete its work — it leaves it running while the front end walks to its next
**synchronous** edge, where the residue is charged instead.

**This is directly observable as MIGRATION.** DC2 G640, `dragon` static tail, per-edge caller-side
wait in ms/f:

| arm | render | readcolor | writecolor | other | **blocked** | cpu |
|---|---:|---:|---:|---:|---:|---:|
| synchronous (control) | **3.35** / 49 | 2.14 / 6 | 0.29 / 7 | 4.30 / 20 | **10.80** | 17.92 |
| async, depth 1 | **1.46–1.57** / 21 | 2.40–2.46 | 0.24–0.27 | 4.91–5.07 | 9.75–10.08 | 18.74–19.33 |
| async, depth 4 | 2.08–2.09 / 21 | 2.48–2.54 | 0.27 | 5.07–5.20 | **10.63–10.83** | 19.17–19.20 |

28 of 49 render round trips genuinely lose their wait (**−1.8 ms/f at that edge**, and the packets
really are complete at drain time — the drain itself measures 11.8 µs). But `readcolor` +0.3 and
`other` +0.7 absorb most of it, `cpu` rises by what `blocked` gave up, and the frame does not move.

⛔ **A deeper ring makes it WORSE, not better.** At depth 4 every remaining synchronous edge queues
behind up to four in-flight packets, so `render` climbs back to 2.09 and the total exceeds control.
"Keep multiple independent workloads in flight" is only a win if the *backend* can execute them in
parallel; with one worker thread it is pure queueing.

---

## 3. The two attribution traps that manufacture the wrong premise

**3.1 A timing bracket named after one call can contain the whole function.** DC2 carried
*"the GS worker blocks 9.925 ms/f inside `gpuOk`"* for three phases and set a −9.0 ms/f target on it.
`gpuOk`'s clock brackets the **entire successful flush attempt** — batch building, materialization,
texture decode, state freeze, window publication AND the submit. The blocking part was 3.0 ms/f.

> **Bracket the wait itself, on the caller, at every edge — and cross-check it against a clock
> inside the worker.** Two instruments that disagree are how you find §2's two terms.

**3.2 A census bucket whose last test is `else` absorbs every item type added after it was written.**
DC2's backend census classifies `pageMap|map|view|colorWrite → 3`, `copyRects → 2`,
`colorReadback|depthReadback → 1`, **`else → 0 "render"`**. A compute-dispatch item added years later
matches none of them, so one 4.2 ms compute was booked as a render and made 47 real renders (~36 µs
each) look like 125 µs each.

> **Before attributing a bucket, read its CLASSIFIER.** The tell that found it: a second census
> enumerated bucket 3's four subtypes and its total matched bucket 3 exactly — so the compute was
> not there, and only the `else` bucket was left.

---

## 4. Why the obvious ceiling probe cannot price this

The natural probe is "delete the round trip at one edge and report success", keeping the whole front
end intact. On a route where a failed submit falls back to a CPU replay, **that probe changes the
population**: a skipped render returns `true`, so a batch that would have fallen back becomes a
"successful" GPU batch. DC2 G640 measured `fallback` **0.38/f candidate against 14.44/f control** and
`other` 13.7 against 1.0 — and because the corruption is stateful, the *control* frames inside the
same process drifted too.

> **A corrupting ceiling probe is only a price with a workload invariant, and you must read the
> invariant before the delta.** When it fails, use a behaviour-pure census instead (§5).

---

## 5. ⭐ The instrument that decides the whole direction in one run: the OVERLAP-WINDOW CENSUS

An async break at edge *E* can hide at most `min(E's own wait, the runnable CPU the front end
executes before it blocks again)`. So:

```
ceiling(K) = SUM over items of min( wait_i , cpu until the K-th next BACKEND edge after item i )
```

where **K is the pipeline depth**, so one run prices the ring depth before anyone builds a ring.

**Implementation notes that matter (DC2 `[G640:ovl]`, `g640_overlap_census.inc`):**

- It rides inside the existing per-edge wait scope, so it costs one extra clock read and a
  small ring update per edge. It deletes, skips and reorders nothing — unlike §4's probe.
- Every blocking edge is fed to it, but only the **backend** edges are BARRIERS. Lock acquisitions
  inside the worker loop (DC2: ~1,886/frame at ~0.24 µs) must have their wait excluded from the CPU
  accumulator yet must **not** close a gap — otherwise every gap reads as nothing.
- Take `min()` **per item**, not on the per-edge averages: one 4.2 ms item among nineteen 5 µs ones
  is invisible to an average.
- Keep a K-deep ring of completed edges and credit each earlier edge when the K-th subsequent
  barrier arrives.

**DC2 result (`dragon` static tail, control):** `ceilK1 = 5.46 ms/f`, `ceilK4 = 8.36 ms/f` of a
10.08 ms/f blocked total. **The CPU to overlap with existed** — which refuted the standing folklore
("the packet had no CPU to overlap with", true only of the lean route it was measured on).

⚠️ **Two mandatory caveats, both learned the hard way:**

1. **This is the CONSUMER-side bound only.** It says how much CPU the front end has; it says nothing
   about the backend's capacity to absorb the deferred work. **Take the minimum of it and
   `backend_serial_busy` (§2).** DC2's `ceilK1 = 5.46` against a scheduling prize of 1.4 is exactly
   the gap between the two bounds.
2. **`gap` can contain the edge's own CONSUMER.** The CPU immediately after a readback is often the
   unpack of that readback, which is *not* independent work. Only an edge whose epilogue is
   provably pure bookkeeping yields a clean number. State which edges you are claiming.

---

## 6. What to do instead — attack the backend's occupancy

If §2 says the prize is small, the question changes from *"who waits?"* to *"what is the backend
actually doing?"* Split `backend_serial_busy` by item type. DC2 G640:

| backend work | ms/f | share |
|---|---:|---:|
| one compute dispatch + its blocking 1 MiB `glGetTexImage` | ≈4.2 | 46% |
| colour readback (`glReadPixels`) | 2.6–2.8 | 30% |
| the actual render batches (47 of them) | ≈1.7 | 19% |
| view/composite/upload builds | 0.33 | 4% |

⭐⭐⭐ **≈77% of it was GPU→CPU readback**, which exists only because guest VRAM must be authoritative
on the CPU. That is a **residency/authority** problem, not a scheduling one — the fix is to keep the
surface on the GPU and make guest memory lazily authoritative, not to reorder submissions.

⚠️ **A partial version of that is usually worse than nothing.** Deferring only the CPU-side
write-back requires settling the pending surface at *every* reader — including CPU-side texture
fetches that never touch the backend. If the target is also sampled as a texture (DC2's shadow
target is), that is a full multi-writer enumeration; getting it wrong is silent corruption.

---

## 7. Micro-packet coalescing (the command-graph half) — price it against DRAW COST, not packet count

A route submitting thousands of tiny GIF packets per frame looks like an obvious coalescing target.
Price it first:

- DC2 `dragon`: **8,044 GIF packets/frame** → but only **47 render batches** reaching the backend,
  costing **≈1.7 ms/f of GL work total (~36 µs each)**. Coalescing draws can reach that 1.7 and no
  more.
- The packet count's real cost was on the **front end** (`31 ms/f` of GIF parsing), which is a parser
  problem with a completely different lever set.

> **A packet count is not a draw cost.** Convert it to backend items and backend ms/f before
> designing a graph. The same arithmetic downgraded DC2's command-graph priority from a −2.0…−4.0
> ms/f target to ≤ −1.7.

Coalescing rules, if the price survives: same framebuffer + blend + texture state merges into one
multi-draw; track write intervals per target and insert a barrier only when a read intersects an
uncommitted write; merge scissors only when identical or subsumed.

---

## 8. Checklist before proposing async/ring/graph work

1. Measure `pole_blocked` (caller side, per edge) **and** `backend_serial_busy` (inside the worker).
   Their difference is your entire scheduling budget. If it is under the route's noise, stop.
2. Read every census bucket's classifier for an `else` branch, and every timing bracket for what it
   actually encloses.
3. Run the overlap-window census (§5) and take the minimum of it and `backend_serial_busy`.
4. Split `backend_serial_busy` by item type. If readback dominates, the phase is a residency phase,
   not a scheduling phase (§6).
5. If you build it anyway: keep an eligibility census, a per-arm workload invariant, and a
   `submitted / drained / failed / ring-full` counter — DC2's read `failed=0 ringFull=0` over 102,001
   packets, which is what let the null be attributed to the architecture rather than to a bug.
6. Gate order-balanced, and **read the printed common window and per-arm present count first** — an
   arm that self-terminates shrinks the gate silently instead of failing it.
