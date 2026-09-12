// ==============================================================================================
// G737 — ROUTE-CENSUS API (shared between the hot call site and the cold implementation)
// ==============================================================================================
//
// The implementation lives in `ps2_g737_route_census.inc`, included by the COLD
// `ps2_g713_pipeline.cpp`. The hot translation units include only this file, so a hot TU pays
// three extern declarations and nothing else (G710: compile-time-dead text in a hot TU measured
// +0.362 ms/f, which is larger than most levers this project ever shipped).
//
// Everything here is a POD of scalars on purpose: the cold TU must not need `G144Entry`,
// `G261Res`, or any GS header to classify a refusal.
#ifndef PS2X_G737_ROUTE_CENSUS_API_INC
#define PS2X_G737_ROUTE_CENSUS_API_INC

#include <cstdint>

// One rejected direct-FBO-bind, as seen at the `kG261MatTexAlias` materialize site. Every field is
// an INPUT to the bind predicate at rasterizer_vram_materialization.inc, so the classifier below
// can name the FIRST failing conjunct without the call site pre-deciding anything.
struct G737TexRejectFacts
{
    uint32_t dstFbp;      // the batch's own FRAME fbp (the consumer)
    uint32_t tgtFbp;      // the resident target that has to be published
    uint32_t texPsm;      // TEX0.PSM
    uint32_t texTbp0;     // TEX0.TBP0, in blocks
    uint32_t tgtBlock;    // framePageBaseToBlock(tgtFbp) — what TBP0 must equal to bind
    uint32_t texTbw;      // TEX0.TBW
    uint32_t tgtFbw;      // the target's FBW
    uint32_t primType;    // GS_PRIM_*
    uint32_t tgtRangeLo;  // the target's block range (0/0 when invalid)
    uint32_t tgtRangeHi;
    uint32_t texRangeLo;  // the CONSUMER's texture block range (g144TextureRange(e))
    uint32_t texRangeHi;
    uint8_t  texRangeValid;
    int32_t  texW, texH;
    int32_t  tgtFbW;      // the target's defined pixel width
    int32_t  covLo, covHi;      // the FBO-defined row window
    int32_t  dirtyLo, dirtyHi;  // the rows this publication will actually read back
    int32_t  u0, u1, v0, v1;    // FST texel extent over the primitive's vertices
    uint8_t  fst;
    uint8_t  bilinear;
    uint8_t  dstIsResident;     // g248TargetIndex(dstFbp) >= 0
    uint8_t  rangeValid;
};

// ⛔⛔ G623's producer-surface bind keeps a BLOCKED-REASON histogram that is counted unconditionally
// (`g_g623Blk[blocked].fetch_add` in g623Decide, reached because `g623Tracked()` includes the
// default-ON `g623ProdSrcOn()`), and whose PRINTER and printer's CALL SITE are both
// `#if defined(PS2X_G684_HOT_DIAG)`. Every release build sets that OFF, so the numbers accumulate
// for the whole run and are then thrown away. This snapshot lets the cold 60-frame reporter print
// them without putting one byte of formatting text back into the flush TU (G710).
struct G737G623Snapshot
{
    uint64_t n;             // entries whose texture base IS a produced surface's base
    uint64_t clean;         // ... of which the bind was admitted
    uint64_t binds;         // binds actually taken
    uint64_t unconsidered;  // entries the pre-pass never saw
    uint64_t blk[16];       // by kG623Blk* reason
    int      blkCount;
    // G737 sampled-rectangle T8 map (g737BuildT8MapRect): how often the rectangle proof succeeded
    // where the whole-texture proof would have refused, and how many texels that saved.
    uint64_t uvProved;   // admissions the rectangle rescued (the whole-texture proof had refused)
    uint64_t uvBuilt;
    uint64_t uvRefused;
    uint64_t uvTexels;
    uint64_t uvFullTexels;
};
void g737_g623_snapshot(G737G623Snapshot &out);
const char *g737_g623_block_name(int i);

bool g737TexRejectOn();
void g737NoteTexReject(const G737TexRejectFacts &f);

// The same row-scope ceiling for `kG261MatCpuReplay` — the WIDEST cause (446 rows/call, 34.2% of
// all published rows on `dungeon1`). `bandBlk*` is the union of the entry's color / depth / texture
// / CLUT block ranges clipped to the target, i.e. every block this entry can touch; `bandValid` is
// false when any of them is unknown, which must be charged in full (fail closed).
void g737NoteCpuScope(uint32_t baseBlk, uint32_t fbw, int dirtyLo, int dirtyHi,
                      uint32_t bandBlkLo, uint32_t bandBlkHi, bool bandValid);

// ⭐⭐⭐ `g144TargetRange(e)` builds an entry's COLOUR-WRITE range from the whole SCISSOR, not from
// the primitive's own extent — while `G144Entry` already carries `minY`/`maxY`, "screen-space bbox
// rows (post-scissor)", computed and shipped for the band-overlap prefilter.
//
// `[G266:cpuShape]` measured the consequence: **2291 of 2396 CPU-replay publications (95.6%) are
// ONE shape** — an untextured SPRITE into `0x13b`, `hit=0x1` (colour), `rej=none`, publishing the
// whole 446-row window of resident target `0x139`. `rej=none` means nothing was refused: the
// publication is a RANGE-OVERLAP decision, and the range is the scissor's.
//
// This prices the narrowing before anything is built (G629's law, and §4.7's lesson — the last
// widening in this phase conserved to the row). `scissorHit` is what the shipped predicate decided;
// `narrowHit` is what it would decide from `[minY..maxY]`. `scissorOnly` — true minus narrow — is
// the only population a narrowing could remove, and a publication it removes is one no entry ever
// needed, which is NOT the shape G304 conservation applies to.
void g737NoteCpuColorNarrow(bool scissorHit, bool narrowHit, int rows);

// ⛔ `g598TryT8EntryAny`'s own reject counters are ALL gated on `g598T8WhyOn()`, which is
// `constexpr false` unless PS2X_G684_HOT_DIAG — the seventh instrument in this family that a
// release build cannot read. `[G598:t8]` prints under the live `DC2_G598_T8ANY` arm but every
// `rej(...)` field in it is therefore structurally zero: a run measured `admit=194023
// rej(shape=0 noSrc=0 clut=0 map=0)`, which is not a census, it is the compile option.
// This is the ship-config replacement, at the same four exits.
enum G737T8Reason
{
    kG737T8ShapeBilinear = 0, // the conjunct this phase found blocking (`!st.bilinear`)
    kG737T8ShapeOther,        // psm / prim / texKey / skip / dimension cap
    kG737T8NoSrc,             // no dirty resident target owns this texture base
    kG737T8Clut,              // CLUT range invalid, or it overlaps a dirty target
    kG737T8Map,               // g281BuildT8Map's fail-closed per-page ownership proof refused
    kG737T8Admit,
    kG737T8ReasonCount
};
void g737NoteT8(int reason, uint32_t dstFbp, uint32_t tbp0, uint32_t tbw, uint32_t psm,
                int texW, int texH, uint32_t prim, int fst, int bilinear);
void g737ReportRouteCensus();

// The G446 PC sampler's joint (out-of-exe leaf x our call site) table. `unwCaller` is the G733
// RtlVirtualUnwind frame (0 when that is not armed or found nothing), `scanCaller` the G503 stack
// scan's answer; the implementation prefers the unwind and records which it used.
bool g737PairsOn();
void g737NotePair(uint64_t leafRip, uint64_t unwCaller, uint64_t scanCaller);
void g737ReportPairs(const char *label, double denom, uint64_t exeLo, uint64_t exeHi);

#endif // PS2X_G737_ROUTE_CENSUS_API_INC
