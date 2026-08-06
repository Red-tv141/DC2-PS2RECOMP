// ==============================================================================================
// G512 — ROW-BATCHED swizzled VRAM readers for the whole-texture decode.
//
// WHY THIS FILE EXISTS.  `g178DecodeWhole` (rasterizer_command_graph.inc) decodes ~81,601 texels
// per call, 4 calls/frame, and G511 priced the whole thing at 1.09 ms/f = ~26.6 ns of CPU per
// texel (~80 host cycles) with 8 row-pool lanes already running.  The G512 disassembly of
// `decodeRows` (image 0x1403db080, 0x260 bytes) showed why: EVERY texel paid an out-of-line
// `call` into this translation unit for the swizzled read — `/GL` + `/LTCG` did NOT inline it —
// plus, for palettized formats, a SECOND out-of-line call for the CLUT lookup.
//
// WHAT THESE DO.  Exactly one call per ROW instead of one per texel.  The body is literally
// `PixelStorageTraits<psm>::Read(...)` in a loop — the SAME function the per-texel readers call,
// in the SAME translation unit, against the SAME live PageTable — so the result is byte-identical
// by construction, not by argument.  Because the call now happens once per row, everything the
// per-texel form had to re-derive across the call boundary (the page-table row base, the block id,
// the page stride, `bp/32`, `y/pageH*bw`) is loop-invariant here and the compiler hoists it.
//
// EXACTNESS.  These are pure readers.  No format is reinterpreted, no mask changed, no address
// re-derived by hand.  `x` runs [0, w) in ascending order, which is the same set of coordinates
// and the same `y` the per-texel loop used.  Anything a caller must special-case (tbw==0 -> 1,
// the T8/T4 page-bw halving, the live-psm dispatch inside `readTexelPSMCT16`) stays the CALLER's
// job and is unchanged — see the G512 block in rasterizer_command_graph.inc.
//
// This file is included INSIDE `namespace GSMem`.  The symbols are deliberately NOT added to
// runtime/ps2_gs_memory.h — editing a standard header costs a 30+ hour full rebuild — so the one
// consumer declares them with a matching `namespace GSMem { ... }` extern block instead.
// ==============================================================================================

template <PixelStorageMode psmV, typename TableT, typename OutT>
static inline void g512ReadRowImpl(const TableT &table, u8 *data, u32 bp, u32 bw, u32 y, u32 w,
                                   OutT *out)
{
    using T = PixelStorageTraits<psmV>;
    for (u32 x = 0; x < w; ++x)
        out[x] = static_cast<OutT>(T::Read(table, data, bp, bw, x, y));
}

void G512ReadRowCT32(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u32 *out)
{
    g512ReadRowImpl<C32>(PageTableC32, data, bp, bw, y, w, out);
}

// The four 16-bit readers (C16 / C16S / Z16 / Z16S) are DELIBERATELY ABSENT. The decode's
// CT16/CT16S arm picks its reader from `gs->activeContext().tex0.psm` — the LIVE GS register —
// and `GS::activeContext()` is private to `friend class GSRasterizer`, so the consumer (a free
// function in the rasterizer TU's anonymous namespace) cannot reproduce that dispatch without a
// header edit, i.e. a 30+ hour full rebuild. Those two formats keep the reference per-texel body.
// Adding readers nothing calls would be dead exported code (G490); write them WITH the caller.

void G512ReadRowP8(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g512ReadRowImpl<P8>(PageTableP8, data, bp, bw, y, w, out);
}

void G512ReadRowP4(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g512ReadRowImpl<P4>(PageTableP4, data, bp, bw, y, w, out);
}

// P4HL/P4HH live in the C32 page layout and differ only in BitOffset (24 / 28) — same as the
// per-texel ReadP4HL/ReadP4HH above.
void G512ReadRowP4HL(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g512ReadRowImpl<P4HL>(PageTableC32, data, bp, bw, y, w, out);
}

void G512ReadRowP4HH(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g512ReadRowImpl<P4HH>(PageTableC32, data, bp, bw, y, w, out);
}
