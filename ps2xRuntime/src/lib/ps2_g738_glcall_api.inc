// ==============================================================================================
// G738 — GL COMMAND-STREAM CENSUS: API (shared between the hot counters and the cold reporter)
// ==============================================================================================
//
// ⛔⛔⛔ WHY THIS EXISTS. `[G496:gl]`, the project's only direct GL-call counter, had two defects
// that together under-stated the GL command stream by roughly an order of magnitude, and the
// resulting number is what closed the call-reduction direction:
//
//   1. ITS DENOMINATOR IS NOT A FRAME. `g496GlNoteFrame()` is called from `renderBatch` on every
//      batch with `!b.skipReadback`. Its own comment says "G495 §2 measured rb/frames = 0.99, so
//      this is the divisor" — that was a different route and a different era. MEASURED on the
//      shipped G732 image, `dungeon1` steady state (n >= 1081): 13 reports over 960 RENDERED
//      frames = 73.8 real frames per report, against a claimed 600 ⇒ **the "frame" is 8.12 real
//      frames**. G704 wrote Rule 44 about this exact reporter ("read a reporter's own frame
//      counter before quoting its rates") and the rate was quoted anyway.
//
//   2. IT COUNTS SIX CLASSES OF CACHED STATE AND NOTHING ELSE. `glDrawArrays` — the single most
//      expensive call in the stream — has no class at all, and neither do `glColorMask`, the
//      uncached `glBindTexture`/`glTexParameteri` on the producer-surface and exact branches,
//      `glActiveTexture`, or the per-batch prologue (program/VAO/VBO/BufferData/FBO/viewport).
//
// ⇒ `issued/f=1044 of 3719` is really **8,482 of 30,217 per frame**, and that is still only the
// cached-state subset. `[G559:runs]` on the same run puts the draw stream at **3,473 physical
// `glDrawArrays` per frame** (2,259 logical draws + the G255 RTT second alpha pass) over 49.7
// batches — not the "34 batches, 849 draws" the G496 header records.
//
// ⛔ The OTHER number that closed the direction is a STAGE TIMER. G690 §1 reported
// "`draw` + `state` is only 0.96 ms/f, refuting multidraw, persistent buffer rings, and
// call-reduction schemes at the ceiling" from `[G415:stage]`. G735 §2 established that a
// `steady_clock::now()` span inside a `/GL /LTCG` PGO TU attributes CODE MOTION, not code — its
// `[G720:cpu] residb` span was charged 2.1 ms/f for a call that fires once per route. A stage
// timer is not admissible as the price of a call class.
//
// WHAT THIS FILE ADDS
//   * a REAL frame denominator: the census is reported from the `[G154:perf]` window on the EE
//     thread, with that window's own rendered-frame count, so every rate is per rendered frame and
//     joins the board by `n` exactly like `[G182:ee]` and `[G734:gsx]`;
//   * full class coverage, including draws;
//   * a DIRECT PRICE for a GL call (`DC2_G738_DRAWBALLAST` / `DC2_G738_STATEBALLAST`): N extra
//     inert GL calls per physical draw, so `Δframe / (N × draws/f)` is nanoseconds per call
//     measured on the thread that actually issues them. That is a mechanism-specific event price,
//     immune to the code motion that invalidates a stage timer.
//
// ⛔ RULE 63 DISCIPLINE. The census and the ballast must NOT be armed together. The census cost
// scales with the call count the ballast changes, so an armed census in a timing arm prices
// itself. Run the ballast arms with `DC2_G738_GLCENSUS` unset.
//
// Everything here is POD scalars: the cold TU must not need a GS header to format a count.
#ifndef PS2X_G738_GLCALL_API_INC
#define PS2X_G738_GLCALL_API_INC

#include <cstdint>

// Call classes. 0..5 are G496's original cached-state classes and keep their numbering so the
// existing cache sites are untouched; 6.. are the ones no census could see.
enum G738GlCls
{
    kG738GlScissor = 0,
    kG738GlDepth = 1,
    kG738GlBlend = 2,
    kG738GlBindTex = 3,
    kG738GlTexParam = 4,
    kG738GlUniform = 5,
    kG738GlDraw = 6,      // glDrawArrays, per PHYSICAL call
    kG738GlColorMask = 7, // glColorMask (the G255 RTT second-alpha-pass triple)
    kG738GlRawBind = 8,   // glBindTexture outside the G496 unit-0 cache
    kG738GlRawParam = 9,  // glTexParameteri outside the per-TexEntry record
    kG738GlActiveTex = 10,
    kG738GlBatch = 11, // per-batch prologue: program / VAO / VBO / BufferData / FBO / viewport
    kG738GlClsCount = 12
};

struct G738GlSnapshot
{
    uint64_t issued[kG738GlClsCount];
    uint64_t elided[kG738GlClsCount];
    uint64_t drawBallastCalls; // extra inert calls the ballast issued (the price denominator)
    uint64_t stateBallastCalls;
    uint64_t batches;    // render batches whose draw loop ran
    uint64_t drawsSeen;  // G178Draw entries visited (LOGICAL draws)
};

// Filled on the EE thread from the GL/executor thread's counters. Defined at GLOBAL scope in
// gpu_raster_bridge_and_stubs.inc — a declaration written inside one of the anonymous-namespace
// parts names an internal-linkage object that is never defined (the C7631 / LNK2001 trap G734
// documented for `g713_exec_busy_ns`).
void g738_glcall_snapshot(G738GlSnapshot &out);

const char *g738GlClsName(int cls);

// The cold reporter, called once per `[G154:perf]` window with that window's REAL rendered-frame
// count. Self-gated on DC2_G738_GLCENSUS; one cached test when off.
void g738ReportGlCalls(uint32_t n, uint32_t windowFrames);

#endif // PS2X_G738_GLCALL_API_INC
