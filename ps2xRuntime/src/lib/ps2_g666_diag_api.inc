// ===== G666: WHY does each DISCOVERED-TARGET batch stay on the CPU, and is its depth exact? =====
//
// WHY THIS FILE EXISTS.
//
// The project's exit condition asks whether any important GS workload remains on the CPU *merely
// because GPU support is missing*, as opposed to being there because the CPU is faster, because
// correctness requires it, or because replacing it is impractical. After G663 the single largest
// residual CPU-fallback population is the DISCOVERED-TARGET family, and the shipped default stacks
// TWO independent refusals on it, adopted in different phases for different reasons:
//
//   1. `g662DiscovNarrowOn()`  -> `g604MeasuredShape` requires `fbw == 2`.   PERFORMANCE-motivated.
//      G662 measured the FBW=8 half at +0.828 ms/f on `ridepod:boss` and withdrew it.
//   2. `g660DiscovNoDepthOnly()` -> the depth arm of `g604ExactDiscov` is disabled outright, so
//      EVERY guest-depth-carrying discovered target is refused.            CORRECTNESS-motivated.
//
// Neither refusal was measured on the population the other one excludes, and the free G666 P0 probe
// (`DC2_G604_STAT=1` on the shipped binary, `ridepod`, 10,560 rejects / 8,983 admits) showed the
// dominant rejected family is `fbp=0x142 fbw=8 guestZ=1 zwrite=1 zbp=0xd0 zpsm=1` — i.e. it fails
// BOTH gates at once, and it is the intersection neither arm has ever measured.
//
// ⭐ THE TWO CENSUSES BELOW ARE THE TWO HALVES OF THAT QUESTION.
//
//   `[G666:discovterm]` — WHICH conjunct actually blocks each batch, as a bitmask, weighted by the
//       entries the batch costs the CPU replay. The critical derived buckets are `widthOnly` (the
//       batch would be admitted if the perf gate opened), `policyOnly` (only the blanket NODEPTH
//       policy blocks it), and `foreignZ` (its depth is NOT the G403/G411 private mirror — the only
//       bucket where GPU support is genuinely absent).
//       ⚠️ The row window is recomputed from the UNCONDITIONAL `g621WrLo/g621WrHi` extent, NOT from
//       `g282RowLo/g282RowHi`: the latter is only accumulated when a candidate predicate already
//       holds, so on a shape-rejected batch it reads `512..-1` and would be scored as an
//       independent failing term when it is really a CONSEQUENCE of the shape gate. (This is the
//       G661 §1.6b rule-2 trap: a classifier whose arming state is itself the thing under test.)
//
//   `[G666:zrate]` — the DEPTH divergence rate at a MATCHED WRITE DEPTH, per FBP.
//       ⛔⛔ THIS EXISTS BECAUSE THE INHERITED `zbad` CANNOT ANSWER THE QUESTION IT WAS USED FOR.
//       `depthBad` in rasterizer_tilebinning_and_probes.inc is
//
//           if (s.g570Depth)
//               for (y = depthRowLo..depthRowHi) for (x = 0..fbW)
//                   if (g403DisplayZRead(x,y,fbp) != s.depthGpu[i]) ++depthBad;
//
//       so (a) it is guarded by `s.g570Depth` and therefore CANNOT FIRE AT ALL for a batch that
//       carries no depth, and (b) its population is the WHOLE depth rect, every untouched pixel
//       included. G660 used it to split the discovered-target population into a "DEPTH/AUTHORITY
//       class" (large zbad) and a "BOUNDED-VALUE class" (zbad = 0) and promoted the blanket NODEPTH
//       refusal on that split — but "zbad = 0" for the exonerated class is a TAUTOLOGY of guard
//       (a), not evidence of exactness, and the ranking across families is a comparison of raw
//       COUNTS over rects of different sizes. This is Rule 20c (which G660 itself wrote, for the
//       COLOUR counter, in the same phase) reproduced on the depth counter.
//       This census re-denominates: it buckets every depth pixel by WHO WROTE IT, using the
//       `depthBefore` snapshot the oracle already captures, and reports divergence against the
//       TOUCHED population rather than the rect. It also reports `badUntouched`, which must be 0 —
//       a nonzero value is a real authority leak and is a different finding from a value error.
//       ⚠️ Rule 26 caveat, stated rather than hidden: "wrote" here is `!= depthBefore`, so a write
//       that reproduces the previous Z exactly is counted as untouched. For a depth buffer under
//       ZTST that is the conservative direction (it can only SHRINK the touched denominator, i.e.
//       it can only make the reported rate look WORSE, never better).
//
// ⛔ RULE 12c. Both call sites are inside `ps2_gs_rasterizer.cpp`, the hottest TU in the build,
// where G653 P25 measured ~60 lines of never-executed `const bool`-guarded code at +0.484 ms/f.
// This is therefore a compile-time build mode (`-DPS2X_G666_DIAG=ON`) with `#if`-EXCLUDED call
// sites. In the default build every symbol below is an empty inline or absent entirely.
//
// Storage and both reporters live in `ps2_g666_diag.cpp`, a cold TU (rule 12b).
//
// ⚠️ The reject branch runs once per BATCH, not per entry, so a map insert under a mutex there is
// far below the noise of the CPU band replay it is describing. The zrate accumulation runs inside
// the oracle's existing per-pixel depth loop, which only executes when a verify flag is armed and
// is already the slowest thing in the process when it is.

#pragma once

#if defined(PS2X_G666_DIAG)

// Failing-conjunct bits of `g604ExactDiscov` (and of the `g604MeasuredShape` contract feeding it).
// A SET bit means "this term was FALSE", i.e. it is one of the reasons the batch was not admitted.
// Mask 0 means every term held.
enum G666DiscovTerm : unsigned
{
    kG666TermWidth        = 1u << 0,  // the fbw gate (narrow-only fbw==2, or the FBW=8 contract)
                                      //   <-- PERFORMANCE-motivated (G662, +0.828 ms/f)
    kG666TermPsm          = 1u << 1,  // entry 0 frame PSM != PSMCT32
    kG666TermMixedEntry   = 1u << 2,  // some entry mixes fbp/fbw/psm or is not sprite/tri/tristrip
    kG666TermRowWindow    = 1u << 3,  // no usable colour row window (from g621, UNCONDITIONAL)
    kG666TermNoDepthPolicy= 1u << 4,  // guestDepth && g660DiscovNoDepthOnly()
                                      //   <-- THE BLANKET POLICY under test
    kG666TermDepthWrites  = 1u << 5,  // guestDepthWrites          <-- correctness-relevant
    kG666TermZbp          = 1u << 6,  // guestZbpPage != 0x0d0     <-- FOREIGN depth authority
    kG666TermZpsm         = 1u << 7,  // guestZpsm != 1 (Z24)      <-- FOREIGN depth authority
    kG666TermZRowWindow   = 1u << 8,  // no depth row window
    kG666TermSharedGpuZ   = 1u << 9,  // !g411SharedGpuDepthOn()
    kG666TermZScope       = 1u << 10, // !g404SharedZScope         <-- FOREIGN depth authority
};

// Defined in ps2_g666_diag.cpp.
bool g666TermCensusOn();
void g666NoteDiscovReject(unsigned fbp, unsigned fbw, unsigned termMask, unsigned zbpPage,
                          unsigned zpsm, int rowLo, int rowHi, int dRowLo, int dRowHi,
                          unsigned prim, unsigned tme, unsigned guestDepth, unsigned depthWrites,
                          unsigned long long entries);
void g666ReportTerms();

// ---- the re-denominated depth oracle -----------------------------------------------------------
bool g666ZRateOn();
// One call per verified depth batch, with the four buckets already reduced by the caller's loop.
//
// ⭐ G667: `hist` is a MAGNITUDE histogram of |cpuZ - gpuZ| over the TOUCHED population, and it is
// the instrument the phase actually turned on. `[G655:depthtrace]` showed the FIRST divergent pixel
// off by exactly 1 LSB on three consecutive batches (`cpu=0001bf2e gpu=0001bf2d`, `zbad=2`) while
// `maxDelta` is 3,790,057 — i.e. the population is bimodal and neither a first-pixel trace nor a max
// can see its shape. A rate plus a max cannot distinguish "a systematic ±1 rounding rule" (fixable,
// cheap) from "a rare unbounded ordering failure" (not fixable), and those two have opposite
// consequences for whether this workload can ever go GPU-native. Buckets are powers of 16 so one
// line covers the whole Z24 range:
//   [0]=1  [1]=2..15  [2]=16..255  [3]=256..4095  [4]=4096..65535
//   [5]=65536..1048575  [6]=>=1048576
// `bigX/bigY/bigCpu/bigGpu` capture the first pixel in bucket >= 4, so a large divergence can be
// located on screen rather than only counted.
enum { kG666ZHistBuckets = 7 };
void g666NoteZRate(unsigned fbp, unsigned prim, unsigned long long rect,
                   unsigned long long cpuWrote, unsigned long long gpuWrote,
                   unsigned long long touched, unsigned long long badTouched,
                   unsigned long long badUntouched, unsigned long long maxDelta,
                   const unsigned long long *hist, int bigX, int bigY,
                   unsigned bigCpu, unsigned bigGpu);
void g666ReportZRate();

#else

// Default build: no storage, no calls, no bytes.
constexpr bool g666TermCensusOn() { return false; }
inline void g666NoteDiscovReject(unsigned, unsigned, unsigned, unsigned, unsigned, int, int, int,
                                 int, unsigned, unsigned, unsigned, unsigned,
                                 unsigned long long) {}
inline void g666ReportTerms() {}
enum { kG666ZHistBuckets = 7 };
constexpr bool g666ZRateOn() { return false; }
inline void g666NoteZRate(unsigned, unsigned, unsigned long long, unsigned long long,
                          unsigned long long, unsigned long long, unsigned long long,
                          unsigned long long, unsigned long long,
                          const unsigned long long *, int, int, unsigned, unsigned) {}
inline void g666ReportZRate() {}

#endif
