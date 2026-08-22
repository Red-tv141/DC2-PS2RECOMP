# Triangle Span Kernels, Lowering & Admission Optimization

> **Core Law**: An untextured rendering path is a mathematical SUBSET of the textured path (no sampler, no CLUT, no STQ coordinate interpolation). A lowering that omits these stages is exact by construction. When an untextured leaf costs MORE than a textured leaf, the cause is an admission conjunct forcing untextured draws into generic fallback loops.

---

## 1. The Accelerated Triangle Replay Hierarchy (G605/G608/G609)

In CPU band replay, replaying GS draw commands through generic per-pixel nests (`writePixel`, `combineTexture`, out-of-line sampling) costs 1,000–3,000 cycles per covered pixel due to ~20 non-inlined function call boundaries per pixel.

Lowered span kernels compile tight, inlined loops that hoist:
- **Private-Z Buffers**: Direct 24-bit depth reads/writes against the G403/G411 mirror at ZBP `0xd0`.
- **Row Scans**: Outlined scanline loops (`g609TriScanRowT`) eliminating per-pixel bounds checks.
- **Exact Half-Up Rounding**: Fast arithmetic replacing CRT rounding (`std::lround`).

---

## 2. Target J: The Untextured Triangle Anomaly (G626)

### 2.1 The Symptom
On `ridepod`, `[G605:leaf]` measured:
- **Textured Triangles (`discov tri-fast`)**: 65.5% of replay CPU, 43.1 px/call, **324.3 cyc/inside**.
- **Untextured Triangles (`discov tri-untex`)**: 30.6% of replay CPU, 11.8 px/call, **961.9 cyc/inside** (3.0× more expensive!).

### 2.2 The Root Cause: Admission Conjunct Refusal
In `g605EntryRejectReason` (`g605_tri_span.inc`):
```cpp
// Defective admission check: refused tme=0 by design
if (!e.prim.tme || !e.prim.iip || e.prim.aa1)
    return kG608EntPrim;
```
Because `tme` (texture mapping enable) was false, all untextured triangles failed admission to the lowered kernel and fell back to the 20-call/pixel generic nest.

### 2.3 Lowering Design for `tme=0` — ✅ SHIPPED (G628), AND THE PLAN ABOVE WAS WRONG TWICE

**What was built** (`DC2_G628_NO_TRI_UNTEX=1` is the rollback; default-ON):
1. `g609TriScanRowT<TEXTURED, ZPRIV, FST, SAMPINL>` — two new template parameters.
   With `TEXTURED == false` the STQ/UV interpolation, the sampler, the CLUT indirection, the bilinear
   quad memo and `combineTexture` are **absent from the instantiation**; the Gouraud shade goes
   straight to the fog lerp and `g605CommitWord`.
2. The admission lives **per-draw in `drawTriangle`**, not in `g605EntryRejectReason`.
3. Result: `discov tri-untex` **952.2 → 501.3 cyc/inside (−47.4%)**, `untexBad=0` on 388,260,458
   per-pixel comparisons via `DC2_G609_VERIFY=1` (not `DC2_G605_VERIFY_TRI`).

⛔⛔ **TRAP 1 — `tme` WAS NOT THE ONLY BLOCKER, AND "every other conjunct already holds" WAS AN
ASSERTION, NOT A CENSUS.** Removing the `tme` refusal admitted **0 of 15,000,073** draws. A per-draw
failing-conjunct BITMASK census then named all three real blockers at once:

| blocked conjunct | what the population actually is |
|---|---|
| `fbw` | **`fbw=2`** — a 128-wide *discovered* target (`fbp=0x141`), not the 512-wide display |
| `ztst` | **`ZTST=ALWAYS`** (`test=0x3000b`), not GEQUAL |
| `privz` | the target does **not** use the G403/G411 private mirror |

⭐ **The law: when a rejection reason is a CONJUNCTION, census the CONJUNCTS.** A first-fail bucket
(`state=14000404`) cannot say which of eleven ANDed predicates blocks a population, nor whether it is
one or nine. Emit a bitmask per draw plus a mask histogram with an overflow counter — that also
proves whether the population is one shape (`rows=2 overflow=0`) or a zoo.

⛔⛔ **TRAP 2 — `g605EntryRejectReason` IS ALSO THE SHARED-CLUT ELIGIBILITY FILTER.** It is handed to
`g580PrepareSharedClutsWith`, which builds one palette per distinct CLUT key and only for keys whose
reference count reaches `minRefs`. Admitting untextured entries there would key a palette off a
`tex0` the draw never reads AND change **which textured draws get a shared palette** — a second
architectural variable on a population worth 65% of the replay. Untextured draws need no palette, so
their admission belongs where the other per-draw conjuncts are read. Leaving the entry predicate
untouched also keeps the textured population identical in both arms, i.e. it becomes the lever's own
same-run control.

⭐⭐ **THE BONUS LOWERING: `ZTST=ALWAYS` + `ZMSK=1` MEANS DEPTH IS DEAD.** The test passes for every
pixel whatever the buffer holds and nothing is written back, yet the generic loop still interpolates
z, clamps it twice and performs a `GSMem::ReadZ24` per pixel before discarding all three. A `ZPRIV`
template parameter deletes the whole stage and takes a **null** depth row, so it is unreachable by
construction. ⚠️ Require the generic path's OWN `zWrite` predicate in the admission, so a
ZTST=ALWAYS draw that *does* write depth fails closed instead of silently losing its Z write.

### 2.4 ⛔ What the lowering did NOT buy, and why

G626 predicted the untextured pixel would land "at or below the textured leaf's 324.3 cyc/inside —
there is no sampler at all". It landed at **501.3**. The residue is not sampling: it is
`insidePerCall = 11.8` against `bboxPerCall = 41.0`. With the sampler gone there is nothing left to
amortise the per-row setup over, and a 12-pixel triangle amortises it over a quarter as many pixels
as a 43-pixel one. **A lowering removes per-pixel work; it does not fix a bad inside/walked ratio.**
That is §3's vein, and it is now the ranked next lever.

⚠️ **AND THE FRAME GATE READ NULL.** `[G605:leaf] LOOP ms` is **per-lane cumulative** across the 8
`GSRowPool` replay lanes, so 24.9 s of deleted lane time is ~0.28 ms/f of wall — below `ridepod`'s
run-to-run spread. Two order-balanced A B B A gates disagreed in sign. Convert lane time to wall time
BEFORE promising a frame delta, and expect every remaining lever of this class to need a
subsystem-scoped estimator rather than a whole-frame one.

---

## 3. Bounding Box Rejection & Span Pruning

Census analysis shows that small triangles often exhibit low bounding box fill ratios:
- `discov tri-fast`: 43.1 inside pixels / 144.1 bbox pixels (**70% rejected**).
- `discov tri-untex`: 11.8 inside pixels / 41.0 bbox pixels (**71% rejected**).

### 3.1 Optimization Rule
When optimizing triangle span loops:
- Retain exact half-plane edge equations inside the loop.
- Pre-compute conservative, tight horizontal spans $[X_{\min}(y), X_{\max}(y)]$ that form a mathematical superset of the true span to skip large blocks of rejected pixels without altering rasterization parity.

⭐ **PRICED BY G628, AND NOW THE RANKED NEXT LEVER.** `[G609:scan]` reports the ratio directly:
`inside/walked = 0.53–0.55` on `ridepod` — **over 45% of the span the kernel actually iterates is
rejected**, on top of the bbox the G141 x-bracket already trimmed. Post-G628 this is the dominant
residue in the untextured leaf (501.3 cyc/inside at 11.8 inside px/call), because deleting the
sampler removed the work that used to amortise the per-row setup.

---

## 4. Moving the whole stage to the GPU (G629) — the kernel is free, the round trip is not

The natural next step after lowering the leaves is to stop replaying on the CPU at all. G629 built
that: an exact GLSL compute replay of the admitted band-replay batch, with the CPU path retained as
oracle and cold fallback.

### 4.1 What held
- **Exactness is achievable.** `bad=0` on **139,606,016** per-word comparisons. The recipe is
  G570/G571's: hand the shader the per-DRAW floats the CPU already computed (edge coefficients,
  `invAbsDenom`, `winding`) instead of re-deriving them, and write every per-pixel expression as
  named `precise` temporaries in the CPU's association order.
- **Admission is wide.** 92.6% of the replay POOL WALL, in **four shapes**
  (`[G629:mask] rows=4 overflow=0`), ≤5 distinct textures per batch.
- **The rasterisation is essentially free**: `dispatch = 0.002 ms/batch` for ~2,000 triangles.

### 4.2 ⛔ What failed, and it was not the kernel
`[G629:split]` mean over 1,536 batches:

| stage | ms/batch | share |
|---|---:|---:|
| upload (~868 KB VRAM window + records + textures) | 0.649 | 32.6% |
| **dispatch** | **0.002** | **0.1%** |
| readback (synchronous `glGetTexImage`) | **1.340** | 67.3% |

End-to-end: **+1.411 ms/f WORSE** (both order blocks agree) against a deletion ceiling of
**−4.93 ms/f**. Two compounding causes:

1. ⭐⭐⭐ **The stage being replaced was 91% WAIT.** `[G529:disp]` = `wall 7.07 / lane0 0.66 /
   rt 6.41 ms/f`. Substituting a GPU round-trip wait for an 8-lane wait does not shorten the
   critical path. **Split any stage into work and wait BEFORE designing a replacement for it.**
2. The synchronous submit blocks the **shared** GL worker queue, serialising the replay against the
   frame's real rendering.

### 4.3 The rule this leaves
⭐ **A GPU replay of a CPU raster stage is a TRANSFER problem, not a rasterisation problem.** Price
the round trip before writing the kernel; if the target cannot be kept GPU-resident across batches,
the kernel's speed is irrelevant.

---

## 5. The DISPATCH around the leaves (G637) — and what it changes about §3's ranking

Everything above prices the *leaf*: cycles per covered pixel. G637 measured the **dispatch** that
feeds those leaves and found the larger number sitting beside them.

### 5.1 The measurement

`[G529:disp]` on `ridepod` (shipped G636 binary, 10,471 frames, 21,600 dispatches):

```
wall 7.12 ms/f   lane0 0.655 ms/f   rt 6.47 ms/f      (rt = wall - lane0 = the caller ASLEEP)
per band: caller 0.318 ms   vs   mean worker band 1.692 ms   (5.3x)
```

`GSRowPool::run` cuts the batch's row range into `lanes` **equal ROW** bands. Equal rows are not
equal work, so the barrier waits for one unlucky band — and the caller is the **GS worker thread**,
which on this route is the sole pole with `gsStallMs/f = 0.00`. **91% of the replay wall was the
pole sleeping.**

Two-magnitude check (`DC2_G529_PXPERLANE=100000000` → `lanes=1`): serialising costs **2.55×**
(3,393 → 8,669 µs/batch), i.e. the 8-way split ran at **32% efficiency**. The parallelism was not
missing; it was being wasted.

### 5.2 The repair, and its two refuted companions

`GSRowPool::runCoop` (`g637_coop_replay.inc`, `DC2_G637_COOP=1`): ~4× more contiguous chunks than
lanes, one generation-tagged lock-free cursor, every participant **including the caller** claims
until the batch is exhausted. Exact by the same argument as the eight-band split (contiguous,
disjoint, whole ordered list clipped to each chunk). Dispatch wall **3.45 → ~0.94 ms**, efficiency
**32% → 82%**, GS own **23.5 → 18.4 ms/f**.

⛔ A **weight-balanced partition** and a **compiled chunk→entry index** were both built and both
failed — the scheduler subsumes the first and the second is a measured null. Full method and the
general laws: `26-cooperative-replay-scheduling.md`.

### 5.3 ⚠️ WHAT THIS DOES TO §3's TARGET K

Post-G637 the dispatch is **aggregate-bound**, not tail-bound (`sum/8 > max` in every swept
configuration). That is good news for §3 — the remaining wall IS the leaves' own work, so a leaf
lever now converts instead of disappearing into a barrier — but it comes with a warning:

⭐⭐⭐ **`ridepod` is no longer a SOLE GS pole with the lever armed.** GS own 18.0–18.8 vs VU1 busy
16.6–17.6 — level within the window spread, headroom down from +4.03 to ~+1.2. **Re-derive the
route's injection sensitivity (`DC2_G431_GS_SLOW_US` / `DC2_G303_VU1_SLOW_US`) before spending a
phase on a GS-only lever**, or §3's win will be capped at ~1 ms by VU1 (`g593_frame_is_max_vu1_gs`).
