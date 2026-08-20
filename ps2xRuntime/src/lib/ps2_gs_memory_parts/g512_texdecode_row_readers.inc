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

// ==============================================================================================
// G620 — the PSMCT32 row reader's GROUP form.
//
// PREMISE (measured before the lever, on `dungeon1`). `[G511:texsplit]` prices the whole-texture
// decode at `0.0549 ms/flush x 45 flushes/f = 2.47 ms/f` — the largest single item in the GS
// front-end preparation (G618 s2a: `pre` 8.79 ms/f against the whole GL backend's 5.13). The G620
// census (`[G620:texdec]`) splits the decoded texels by source format: **PSMCT32 is 61.1% of every
// texel decoded on this route**, dominated by one 512x512 CT32 read of framebuffer page 0x13b
// (`tbp=0x02760`) at 915 k texels/frame.
//
// WHAT WAS LEFT.  G512 already removed the per-texel CALL and the per-texel psm switch, but the
// body it left is still `PixelStorageTraits<C32>::Read` once per texel: a 3-D subscript into the
// 128 KB page table plus the `PageId` divide tree, per pixel, to fetch FOUR bytes.
//
// MECHANISM — the same structural fact G477 already proved and verifies from the table itself.
// `ColumnTable32` places eight consecutive x inside one page row at pixel offsets
// `row[g]+{0,1,4,5,8,9,12,13}`, i.e. FOUR PAIRS OF ADJACENT WORDS at a 16-byte stride. So an
// aligned group of 8 texels is four 8-byte loads at `b, b+16, b+32, b+48` written to 32 contiguous
// output bytes — one table load and four moves for eight texels, instead of eight table-indexed
// address derivations.
//
// EXACTNESS.  Three separate guards, none of them an assumption about the layout:
//   1. the grouping is not assumed — `g477C32GroupOk` VERIFIES `row[g+2k] == row[g]+4k` and
//      `row[g+2k+1] == row[g]+4k+1` for this exact (block, row) pair against the live page table
//      that `Read` itself indexes, once, at first use;
//   2. `pairSafe` reproduces G477's segment guard, so the `& (MEMORY_SIZE - 4)` mask in `Read` is
//      provably the identity over the whole group and `addr+1` is `byte+4`;
//   3. the group form touches bytes `b + 0 .. b + 55` — EXACTLY the range the eight reference
//      reads touch (the last texel of the group is `row[g]+13`, i.e. bytes `b+52..b+55`). It is
//      not an over-read, so it cannot fault where the reference would not.
// Anything the guards reject falls through to the reference per-texel loop for that segment.
//
// Oracle: `DC2_G620_VERIFY=1` re-reads every texel the group path produced through
// `PixelStorageTraits<C32>::Read` and counts mismatches. Rollback: `DC2_G620_NO_FASTROW=1`.
// ==============================================================================================

// Namespace scope, deliberately — a function-local `static const bool` is a per-access
// thread-safe-static guard on MSVC (`_Init_global_epoch` TLS read + compare), which is exactly the
// tax this reader exists to remove (PS2_PROJECT_STATE.md, G619).
static const bool g_g620FastRowCt32 = (std::getenv("DC2_G620_NO_FASTROW") == nullptr);
static const bool g_g620RowVerify = (std::getenv("DC2_G620_VERIFY") != nullptr);
std::atomic<std::uint64_t> g_g620RowVerifyN{0};
std::atomic<std::uint64_t> g_g620RowVerifyBad{0};

void G512ReadRowCT32(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u32 *out)
{
    using T = PixelStorageTraits<C32>;
    if (!g_g620FastRowCt32)
    {
        g512ReadRowImpl<C32>(PageTableC32, data, bp, bw, y, w, out);
        return;
    }

    constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 4u;
    constexpr u32 kPageW = 64u;
    constexpr u32 kPageH = 32u;
    constexpr u32 kPagePixels = kPageW * kPageH;
    constexpr u32 kRowMax = 4031u; // highest value any C32 page-table entry can hold (G477)

    const auto &row = PageTableC32[bp % 32u][y % kPageH];
    const u32 basePage = (bp / 32u) + (y / kPageH) * bw;
    const bool groupOk = g477C32GroupOk<C32>(PageTableC32, bp % 32u, y % kPageH);

    u32 i = 0u;
    while (i < w)
    {
        const u32 pageBase = (basePage + (i / kPageW)) * kPagePixels;
        const u32 idx = i & (kPageW - 1u);
        u32 lim = kPageW - idx;
        if (lim > w - i)
            lim = w - i;
        const bool pairSafe = (pageBase <= (kMask / 4u) - (kRowMax + 1u));

        u32 j = 0u;
        if (groupOk && pairSafe && ((idx & 7u) == 0u))
        {
            while (j + 8u <= lim)
            {
                const u32 b = (pageBase + row[idx + j]) * 4u;
                u32 *o = out + i + j;
                std::memcpy(o, data + b, 8u);
                std::memcpy(o + 2, data + b + 16u, 8u);
                std::memcpy(o + 4, data + b + 32u, 8u);
                std::memcpy(o + 6, data + b + 48u, 8u);
                if (g_g620RowVerify)
                {
                    for (u32 k = 0u; k < 8u; ++k)
                    {
                        const u32 ref =
                            static_cast<u32>(T::Read(PageTableC32, data, bp, bw, i + j + k, y));
                        g_g620RowVerifyN.fetch_add(1u, std::memory_order_relaxed);
                        if (ref != o[k])
                            g_g620RowVerifyBad.fetch_add(1u, std::memory_order_relaxed);
                    }
                }
                j += 8u;
            }
        }
        for (; j < lim; ++j)
            out[i + j] = static_cast<u32>(T::Read(PageTableC32, data, bp, bw, i + j, y));
        i += lim;
    }
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
