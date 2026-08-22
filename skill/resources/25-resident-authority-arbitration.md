# 25 — Resident GPU Authorities: arbitration, enumeration, and when to delete one

> **GENERIC.** Applies to any port that keeps guest video memory (or any guest resource) resident on
> the host GPU instead of in the guest's own address space. Written from the G629–G636 arc, whose
> six phases produced three visual defects, `bad=0` on **1.67 billion** exactness comparisons, and a
> net negative frame result — all from one unstated assumption.
>
> Companions: `17f-ab-gate-and-oracle-traps.md` (oracles that report clean) ·
> `15b-gs-state-and-capture-ab.md` (pixel A/B) · `appendix-<game>-capture-and-gates.md`.

---

## §1 THE LAW

> **A resident authority is only correct if it is a SUPERSET of every writer of the memory it
> claims. So the FIRST artefact of any residency design is an enumerated list of the writers — not
> a shader, not an admission, not an oracle.**

Residency is not "keep the data on the GPU". It is a claim of the form *"for these addresses, my
copy is the newest one."* That claim is false the moment any writer you did not enumerate touches
those addresses, and it is false SILENTLY: every path involved is individually exact, so every
per-path oracle you build will read `bad=0` while the picture is wrong.

### §1.1 The enumeration, done properly

For each writer, record four things. If you cannot fill a row, you have not found the writer yet.

| | question |
|---|---|
| **where** | which host object holds its pixels/bytes before they reach the claimed memory |
| **when** | at which call site they become visible |
| **how the resident domain learns** | the publication/invalidation hook, by name |
| **what happens if it doesn't** | the observable defect |

The G629–G636 arc's table, after the fact:

| writer | where | domain learned via | if not |
|---|---|---|---|
| the CPU band replay | guest VRAM directly | `g630PrepareCpuReplay` / `g630NoteCpuReplay` | ✅ handled from the start |
| Host→Local IMAGE transfers | guest VRAM directly | the upload-complete hook | ✅ handled from the start |
| **the native renderer's per-target FBOs** | `m_fbos[fbp]`, unpublished | ⛔ **nothing** | textures decoded from a stale copy |
| **the CPU's private depth mirror** | a host array | only after a band replay | polygons wrongly depth-rejected |

Two of four. Three defects. **The ratio is typical** — the writers a residency design misses are
precisely the ones that live in *another* host-side cache, because those are invisible to both guest
memory and the resident image.

---

## §2 SYMPTOM → WRITER, WITHOUT A SEARCH

⭐⭐ **Name the writer by its FOOTPRINT before hunting for it in code.** Diff the resident copy
against the guest copy at the moment of divergence and print, in one line: *how many words changed,
their row/column range, and how many went to/from zero.*

| footprint | what wrote it |
|---|---|
| whole surface → a constant | a CLEAR |
| whole surface, values unrelated | a re-seed / re-upload |
| a scissor-shaped band | one draw batch |
| **an object's on-screen silhouette** | **the rasteriser, for one model** |
| a page-aligned rect | an IMAGE transfer |

G636's `[G636:zwho]` printed `changed=11973/262144 rows=161..401 ->zero=0 fromZero=0` — the
silhouette of the exact object that was disappearing. That single line converted an open search over
every Z-writing site in the renderer into a closed one.

---

## §3 THE MERGE TEST — the decision that tells you to DELETE the mechanism

When you find the missing writer, you have three options and only three:

1. **Publish before it** (resident → guest), then let it write. Costs one readback per occurrence.
2. **Invalidate after it** (guest → resident), re-seeding on next use. Costs one upload per
   occurrence.
3. **Delete the residency** for that data.

Options 1 and 2 both assume that at the moment of conflict **only one side has changes**. Measure
whether that is true before designing either:

```
at each resident use:  changed = (guestCopy != shadowOfLastSync)
                       count(changed), and count(changed && residentCopyIsDirty)
```

⭐⭐⭐ **If `changed && residentDirty` is a large fraction of `changed`, both sides hold real writes,
the conflict is a MERGE you cannot perform, and option 3 is the only correct answer.** G636 measured
`1,082 of 1,082` — every single divergence. No detection, no ordering fix and no extra publication
can repair that; the mechanism has to go.

Do not skip this measurement because a fix "obviously" works: G636's shadow-based invalidation is
exactly option 2, is exact by construction, and still left the defect at p90 31 — because it was
re-seeding away the resident side's own writes.

---

## §4 THE FBO-KEY TRAP

⭐⭐ **Check what your framebuffer cache is keyed by before you render into it.**

A backend that keys FBOs by target base address alone (`m_fbos[fbp]`) hands the SAME object to the
native renderer and to any "resident mirror" that writes the same base. Then:

- a compute pass that `imageStore`s **unconditionally over its dispatch grid** — the natural way to
  write one, since the destination read/modify/write is idempotent for uncovered pixels — erases
  every native draw in that rect, because for the FBO the store is not idempotent at all;
- a "view builder" that decodes a whole ROW BAND into it erases the rows outside the part it
  reconciled.

Both are invisible in any per-pixel oracle: the values written are *correct values of the resident
image*. The rule:

> A resident mechanism must render into a **private key**, or prove it is the sole writer of the
> shared one. If it renders into the shared key, it may only write pixels it actually covered, and
> only while the FBO is provably in sync with guest memory for the rest of the band.

---

## §5 SCOPE THE ORACLE TO THE CLAIM

⭐⭐ **An oracle over "everything that could be checked" is worth far less than one over exactly what
the design ASSERTS.** The identical raw-image check in G636:

| population | result | worth |
|---|---|---|
| every page the GPU does not own | `bad = 2,142,594 / 3,538,944` | **none** — most were never claimed and get patched on demand |
| pages where `syncedGen == pageGen` | **`bad = 0` on 291,676** | decisive: the claim holds, look elsewhere |

Write the claim as a predicate in the code, then test that predicate's population. This is the
constructive half of G635's law 2 ("supply the reference on the population that can FAIL").

---

## §6 CLOSING A RESIDENCY VEIN HONESTLY

A residency design usually replaces a stage that is **mostly WAIT on other threads**, not compute.
Before (and after) building it, do this arithmetic:

```
addressable  = (pole-thread wall of the replaced stage) x (share of the population you can serve CORRECTLY)
cost         = publication + added backend + added front-end, all measured on the POLE thread
```

and note that the correctness work *shrinks the numerator*: G636's depth removal was mandatory and
took 57% of the population out of reach, so the addressable ≈1.2 ms/f fell below the ≈1.8 ms/f cost.

Two closure signals that are stronger than any single gate:

- ⭐⭐ **MONOTONICITY.** Gate each mechanism cumulatively. If every increment is negative
  (`+1.129 → +1.383 → +1.506`), the family's optimum is zero mechanisms and no tuning inside it can
  win. That is a closure proof, not a data point.
- ⭐ **The ceiling still exists.** Re-state what deleting the stage outright is worth. If the prize
  is real (G629's −4.93 ms/f) but unreachable through this transport, say exactly that: the vein is
  closed to *this* transport, and reopening needs a new one — never a new admission.

---

## §7 CHECKLIST FOR THE NEXT RESIDENCY DESIGN

1. Enumerate the writers (§1.1). Do not write code until the table has no blank cells.
2. For every writer, name the hook and add it to **every** dispatch path — including the sequential /
   frame-end / fallback closures. G636 found one closure that had never had the contract at all,
   six phases in.
3. Run the merge test (§3) for every shared resource **before** designing an invalidation.
4. Give every resident render target a private key (§4).
5. Write the claim down as a predicate and point the oracle at its population (§5).
6. Gate with PIXELS, cumulatively, one mechanism per arm (§6, and `17f`).
7. Price the addressable share against the measured cost before the second phase, not the sixth.
