// ===== G660 P2/P3/P4: WHY does each `colorzalias` batch die, and is the alias LIVE? ============
//
// WHY THIS FILE EXISTS.
//
//   P2  `[G657:rejwall]` names the family (`colorzalias cls=legacy fbp=0x139`, 67.15% of
//       `lavaboss`'s and 58.32% of `dragon`'s residual CPU-fallback wall) but not the CONDITION.
//       The reject is one boolean:
//
//         colorZAlias = rawColorZAlias && !g282SeededColorZ && !g290Seeded && !g570Exact13d &&
//                       !g586Exact141 && !g587Exact15d && !g570Exact139 && !g604ExactDiscov;
//
//       For FBP 0x139 the only carve-out that can apply is `g570Exact139`, itself a conjunction of
//       ELEVEN terms. Knowing WHICH term is false is the whole difference between a
//       CORRECTNESS-REQUIRED alias restriction (the guest really does read colour it just wrote as
//       depth, or vice versa) and a CONSERVATIVE ADMISSION restriction (the contract holds and the
//       batch is refused only because a default-OFF gate suppresses the exact row windows). This
//       census records the failing conjuncts as a bitmask, per batch, with the batch's entry count
//       so the answer is weighted by the population it actually blocks.
//
//   P3  `rawColorZAlias` is `g242RectsOverlap` over LEGACY GS PAGE GEOMETRY: colour at
//       `framePageBaseToBlock(0x139)` with FBW=8/PSMCT32, depth at `zbpPage << 5` with PSMZ24.
//       On real hardware those page ranges do overlap. In THIS PORT they do not share storage at
//       all: `g404SharedZScope(fbp, 8, 0xd0, 1)` selects the G403/G411 PRIVATE depth mirror, which
//       is a host-side 512x512 `std::vector<uint32_t>` (`g403DisplayZBuf`), deliberately kept
//       OUTSIDE aliased VRAM so loading/menu texture staging can keep block 0x1a00. So the overlap
//       predicate is a statement about a memory map this port does not use. `zAliasLive` records
//       that per batch instead of leaving it as a reading of the source.
//
//   P4  The authority question ("which transition forces CPU replay?") is answered by the same
//       row: when depth is the private mirror and the batch never writes depth (`zmsk=1`), colour
//       and depth have DISJOINT owners and there is no ownership transition to localize.
//       `guestDepthWrites` and `zAliasLive` together are that fact, measured.
//
// ⛔ RULE 12c. The call site is inside `g178TryFlushGpu`'s alias branch in
// `rasterizer_vram_materialization.inc`, compiled into `ps2_gs_rasterizer.cpp` — the hottest TU in
// the build, where G653 P25 measured ~60 lines of NEVER-EXECUTED `const bool`-guarded code at
// +0.484 ms/f. This is therefore a compile-time build mode (`-DPS2X_G660_DIAG=ON`) with an
// `#if`-EXCLUDED call site. In the default build every symbol below is an empty inline or absent.
//
// Storage and the reporter live in `ps2_g660_diag.cpp`, a cold TU (rule 12b).
//
// ⚠️ The reject branch runs once per BATCH, not per entry, so a map insert under a mutex there is
// far below the noise of the CPU band replay it is describing.

#pragma once

#if defined(PS2X_G660_DIAG)

// Failing-conjunct bits of `g570Exact139`. A SET bit means "this term was FALSE", i.e. it is one
// of the reasons the exact carve-out did not apply. Mask 0 means every term held and the batch was
// admitted (or would have been, had the gate been on).
enum G660ExactTerm : unsigned
{
    kG660TermGateOff      = 1u << 0,  // g570Private139On() - the DEFAULT-OFF env gate
    kG660TermFbp          = 1u << 1,  // fbp != 0x139
    kG660TermFbw          = 1u << 2,  // fbw != 8
    kG660TermPsm          = 1u << 3,  // entry 0 frame PSM != PSMCT32
    kG660TermNoGuestDepth = 1u << 4,  // !guestDepth
    kG660TermDepthWrites  = 1u << 5,  // guestDepthWrites  <-- CORRECTNESS-RELEVANT
    kG660TermZbp          = 1u << 6,  // guestZbpPage != 0x0d0
    kG660TermZpsm         = 1u << 7,  // guestZpsm != 1 (Z24)
    kG660TermRowWindow    = 1u << 8,  // g282 colour/depth row window empty
    kG660TermSharedGpuZ   = 1u << 9,  // !g411SharedGpuDepthOn()
    kG660TermZScope       = 1u << 10, // !g404SharedZScope   <-- CORRECTNESS-RELEVANT
};

// Defined in ps2_g660_diag.cpp.
bool g660RejCensusOn();
void g660NoteColorZReject(unsigned fbp, unsigned fbw, unsigned termMask, unsigned zbpPage,
                          unsigned zpsm, int rowLo, int rowHi, int dRowLo, int dRowHi,
                          unsigned prim, unsigned tme, unsigned abe, unsigned zAliasLive,
                          unsigned long long entries);
void g660ReportRej();

#else

// Default build: no storage, no calls, no bytes.
constexpr bool g660RejCensusOn() { return false; }
inline void g660NoteColorZReject(unsigned, unsigned, unsigned, unsigned, unsigned, int, int, int,
                                 int, unsigned, unsigned, unsigned, unsigned, unsigned long long) {}
inline void g660ReportRej() {}

#endif
