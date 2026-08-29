// ===== G662: the phase's COMPLETE instrument set, frozen before the first diagnostic build =====
//
// G661's retrospective (skill/SKILL.md §1.6b rule 1) cost that phase three diagnostic relinks
// because each new priority suggested one more census. Every question G662 intends to ask of the
// runtime is declared here, in one file, before anything is compiled.
//
//   [G662:zauth]    P10. WHICH FBPs write the G403/G411 private depth mirror from a CPU-REPLAYED
//                   entry? Single-authority GPU depth ownership is available only if the writers
//                   are confined to the families the design would move onto the GPU.
//                   ⛔ G661 asked this from inside `g261PrepareCpuReplay`, which is reached only
//                   after `if (!g261WaveOn() || !g261AnyDirty() || g_g144List.empty()) return;` —
//                   so it saw CPU-replay entries only in flushes where a resident target was
//                   ALREADY dirty and reached `cpuEntriesSeen=1` on `combat` and `ridepod`. This
//                   one is called at the real CPU band-replay choke point, immediately after
//                   `g178TryFlushGpu` returns false, where EVERY entry about to replay on the CPU
//                   passes exactly once.
//                   Same two anti-lying properties G661 fixed and this keeps: no predicate (a raw
//                   per-FBP histogram cannot lie about its own classifier — G661 §1.6b rule 2),
//                   and the DENOMINATOR is counted unconditionally with the report keyed to IT,
//                   so a genuine ZERO — this probe's single most important possible answer — is
//                   distinguishable from a route that never reached a reporting period
//                   (G657 measurement law 10, G661 §1.6b rule 3).
//
//   [G662:matgran]  P6. The roadmap asks for "per-row Local publication work" to be replaced by
//                   batched transfers. Reading `g261Materialize` shows there is no per-row
//                   transfer to replace: one `g178_backend_read_color` over the whole dirty span,
//                   one `g418UnpackColorRows`, one `g178BumpRectImpl`. The remaining batching
//                   question is therefore across CALLS, not within one: how many kG261MatLocal
//                   publications does a single flush edge take, on how many distinct targets, and
//                   how many rows each. That is what this counts.
//
//   [G662:lever]    Arm counters for every G662 lever, printed at n==1 and periodically, so a
//                   null A/B is a MEASURED null rather than an unobserved one
//                   (G657 measurement law 10 / G659 P18).
//
//   [G662:colspan]  P6-follow-on. G621 asked "how many of the rows this publication transfers does
//                   the batch actually write?" and measured `dungeon1` reading **416 rows to write
//                   89.7 (keep=21.6%)** — then narrowed the ROW window on that evidence. The COLUMN
//                   axis has never been asked, and the readback is `g178_backend_read_color(k,
//                   r.fbW, ...)`: ALWAYS THE FULL TARGET WIDTH, whatever the batch's scissor was.
//                   P6 measured that readback at **80.05% of the whole `kG261MatLocal` cost** on
//                   `dragon`, so a column bound is Rule 11 (producer-bounded transfers) applied to
//                   the largest single term in the publication.
//                   This measures the ceiling BEFORE anything is narrowed: per publication, the
//                   scissor-union column extent against `fbW`, bucketed, with the pixel counts
//                   both ways so the prize is a ratio of TRANSFERRED PIXELS and not of rectangles.
//                   ⛔ If `colFrac` is ~1.0 the batches really are full-width and there is no
//                   prize — that is the answer this must be allowed to give.
//
// ⛔ RULE 12c. Both call sites are inside `ps2_gs_rasterizer.cpp`, the hottest TU in the build,
// where G653 P25 measured ~60 lines of never-executed `const bool`-guarded code at +0.484 ms/f.
// So this is a compile-time build mode (`-DPS2X_G662_DIAG=ON`) with `#if`-EXCLUDED call sites; in
// the default build every symbol below is an empty inline or absent. Storage and the reporters
// live in `ps2_g662_diag.cpp`, a cold TU (rule 12b).

#pragma once

#if defined(PS2X_G662_DIAG)

// Lever ids for [G662:lever]. One per runtime arm this phase can select.
enum G662LeverId : unsigned
{
    kG662LeverBlend139 = 0, // P2: the map125 0x139 blend-5/6 classifier widening
    kG662LeverCount
};

// Defined in ps2_g662_diag.cpp.
void g662NoteCpuReplayEntry(unsigned fbp, unsigned long long zbuf);
void g662NoteMatLocal(unsigned fbp, int rowLo, int rowHi);
void g662NoteLever(unsigned id);
void g662NoteColSpan(unsigned fbp, int fbW, int colLo, int colHi, int rows);

#else

// Default build: no storage, no calls, no bytes.
inline void g662NoteCpuReplayEntry(unsigned, unsigned long long) {}
inline void g662NoteMatLocal(unsigned, int, int) {}
inline void g662NoteLever(unsigned) {}
inline void g662NoteColSpan(unsigned, int, int, int, int) {}

#endif
