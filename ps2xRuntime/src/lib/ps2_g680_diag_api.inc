// ===== G680 diagnostic API — the item-menu ICON SPRITE geometry census =======================
//
// ONE question: what is the EXACT affine map of a scaled FST sprite? G680 has already proved the
// icon draws paint the black dashes and exonerated the artwork, the CLUT, the filter and the
// requested sizes. What has never been printed is the map itself — the 12.4 screen rect, the 12.4
// UV rect, the front end's -0.5-texel FST shift, and the G406/G407/G679 rounding state — which is
// what decides which texel row each fragment reads.
//
// ⛔ RULE 12c. The call site is inside `ps2_gs_rasterizer.cpp`, the hottest TU in the build, so
// this is a compile-time build mode (`-DPS2X_G680_DIAG=ON`) with an `#if`-EXCLUDED call site; in
// the default build the symbol is an empty inline. Storage and the reporter live in
// `ps2_g680_diag.cpp`, a cold TU (rule 12b).

#pragma once

#if defined(PS2X_G680_DIAG)

// Defined in ps2_g680_diag.cpp.
void g680NoteSpriteSnap(int snapped, int tme, int fst);
void g680NoteSpriteOracle(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1);
void g680NoteIconSprite(unsigned tbp, unsigned tbw, unsigned psm,
                        int texW, int texH, int pushTexW, int pushTexH,
                        int x0, int y0, int x1, int y1,
                        int u0, int v0, int u1, int v1,
                        int qx0, int qy0, int qx1, int qy1,
                        double shU0, double shU1, double shV0, double shV1,
                        unsigned flagsU, unsigned flagsV, int originX, int originY,
                        unsigned periodU, unsigned periodV,
                        int screenSprite, unsigned wrapU, unsigned wrapV, int cpuPath);

#else

inline void g680NoteSpriteSnap(int, int, int) {}
inline void g680NoteSpriteOracle(int, int, int, int, int, int, int, int) {}
inline void g680NoteIconSprite(unsigned, unsigned, unsigned, int, int, int, int,
                               int, int, int, int, int, int, int, int, int, int, int, int,
                               double, double, double, double,
                               unsigned, unsigned, int, int, unsigned, unsigned,
                               int, unsigned, unsigned, int) {}

#endif
