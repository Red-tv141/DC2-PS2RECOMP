// ===== G663 diagnostic API - the bind-pre-pass SCAN population census ========================
//
// ONE question, asked before its lever is believed: the bind pre-pass walks all 14 residency slots
// PER ENTRY, and the hoist that walks only the dirty ones is exact - but exactness is not value.
// If most slots were dirty most of the time the hoist would save nothing. `[G663:bindscan]` counts
// the dirty-slot population per flush and the scan iterations both ways, so the ceiling is a
// measured number before any timing arm is run.
//
// Measured premise this exists to price (promoted binary, G663 P2):
//   [G323:census] bind = 2.838 ms/f on dragon:tail, 3.754 ms/f on s03:steady - the largest
//                 convertible CPU term in the whole `exec` partition.
//   [G656:bindstage] tail = 60.84% / 56.29% of that loop, ~1,344 cyc per entry reaching it.
//   The tail's dominant term is `for (t = 0; t < kG248TargetCount; ++t) ... if (!r.dirty) continue;`
//   at 18.3 M entries x 14 slots on one dragon run.
//
// ⛔ RULE 12c. The call sites are inside `ps2_gs_rasterizer.cpp`, the hottest TU in the build,
// where G653 P25 measured ~60 lines of never-executed `const bool`-guarded code at +0.484 ms/f. So
// this is a compile-time build mode (`-DPS2X_G663_DIAG=ON`) with `#if`-EXCLUDED call sites; in the
// default build every symbol below is an empty inline or absent. Storage and the reporter live in
// `ps2_g663_diag.cpp`, a cold TU (rule 12b).

#pragma once

#if defined(PS2X_G663_DIAG)

// Defined in ps2_g663_diag.cpp.
bool g663DiagOn();
void g663NoteBindScan(unsigned dirtyN, unsigned long long entries, bool armed);
void g663NoteTail(unsigned long long texpubCyc, unsigned long long scanCyc);

#else

// Default build: no storage, no calls, no bytes.
inline bool g663DiagOn() { return false; }
inline void g663NoteBindScan(unsigned, unsigned long long, bool) {}
inline void g663NoteTail(unsigned long long, unsigned long long) {}

#endif
