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

// ==============================================================================================
// ⭐ G650 (ROADMAP Priority 5) — the PALETTED row readers, with the address chain hoisted
//
// PREMISE (G620's own, re-read). `[G511:texsplit]` on `dungeon1` prices the whole-texture decode at
// `0.0549 ms/flush x 45 flushes/f = 2.47 ms/f` — the largest single item in the GS front-end
// preparation. G620 took the PSMCT32 share of it (61.1% of decoded texels) with the aligned-group
// reader above. The PALETTED share was left on `g512ReadRowImpl`, which is still
// `PixelStorageTraits<psm>::Read` ONCE PER TEXEL — and that call re-derives, per texel:
//     PageId(block, bw, x, y)        two divides and a multiply,
//     block % 32, y % pageH, x % pageW,
//     page * PixelsPerPage(),        a multiply,
// all of which are LOOP-INVARIANT once the row is sliced at page boundaries. The only thing that
// actually varies across a slice is `table[block][y][x]`, one u16 load.
//
// ⭐ THE MECHANISM, and why it is not "SIMD". ROADMAP P5 proposes `_mm256_i32gather_epi32` for the
// CLUT lookup. That is the wrong half: the CLUT lookup is already `clut[src[x]]` against a 1 KB
// table that is hot in L1 (G512 pre-builds it once per decode), and an AVX2 dword gather retires no
// faster than the scalar load it replaces. The cost is the ADDRESS DERIVATION, and the fix for that
// is hoisting, not vectorising. Where the page table does turn out to be contiguous over a group,
// the hoisted form additionally collapses to a straight `memcpy` — which is the widest possible
// move and needs no ISA extension at all.
//
// EXACTNESS. Three guards, none of them an assumption about the layout:
//   1. the byte address is the SAME expression `Read` computes, specialised per psm by hand and
//      stated in each arm below (P8: `pa`; P4: `pa>>1` + nibble `(pa&1)*4`; P4HL/P4HH: `pa*4+3`
//      with shift 0 / 4). The `& (MEMORY_SIZE - 1)` mask is kept verbatim.
//   2. the contiguous-group arm is not assumed — `g650PalGroupOk` VERIFIES `row[g+k] == row[g]+k`
//      for every aligned group of 8 in this exact (block, page row) against the live page table,
//      once, at first use; a row that fails takes the scalar hoisted arm.
//   3. the group arm needs the mask to be the identity across all 8 bytes, which `groupSafe`
//      checks against the largest entry the table can hold.
//
// Oracle: DC2_G650_ROWPAL_VERIFY=1 re-reads every produced texel through
// `PixelStorageTraits<psm>::Read` and counts mismatches (`[G650:rowpal]`).
// Rollback: DC2_G650_NO_ROWPAL=1 restores `g512ReadRowImpl` verbatim.
// ==============================================================================================

// Namespace scope — a function-local `static const bool` is an MSVC per-access TLS guard, i.e. the
// exact tax these readers exist to remove (PS2_PROJECT_STATE.md, G619).
static const bool g_g650RowPalOn = (std::getenv("DC2_G650_NO_ROWPAL") == nullptr);
static const bool g_g650RowPalVerify = (std::getenv("DC2_G650_ROWPAL_VERIFY") != nullptr);
std::atomic<std::uint64_t> g_g650RowPalN{0};
std::atomic<std::uint64_t> g_g650RowPalBad{0};
std::atomic<std::uint64_t> g_g650RowPalGrouped{0};

// ==============================================================================================
// ⭐ G651 (ROADMAP Priority 2) — the group predicate WIDENED, and the word-pair arm that the
//    C32-resident 4-bit formats actually admit.
//
// PREMISE. G650 shipped the hoisted paletted row readers and measured frame −0.117 / GS own −0.088
// on `dungeon1`, but `[G650:rowpal] grouped=0` over 1.2 G texels: the 8-wide contiguous `memcpy`
// arm NEVER FIRED. ROADMAP P2 asks for 4-wide and 2-wide contiguous group reads on the same
// verified-from-the-live-table basis, plus a byte-pair path for `PSMT4HL`/`PSMT4HH`, which
// `[G512:psm]` prices at **51% of every texel decoded on `dungeon1`** (`psm2c` 438 M of 855 M).
//
// ⛔ WHAT THE TABLE SAYS ABOUT ITEM 1 (and it is a REFUTATION, recorded so it is never re-chased).
// `ColumnTable8` row 0 is `{0,4,16,20,32,36,48,52, 2,6,18,22,34,38,50,54}` — consecutive x are
// **4 apart, not 1**. So `row[g+1] == row[g]+1` is false at EVERY alignment, and a contiguous
// predicate cannot fire at width 8, 4 **or 2**; the same holds for `ColumnTable4`
// (`{0,8,32,40,…}`, stride 8). The predicates below are still BUILT and still CENSUSED at all
// three widths — `[G651:rowpal] ok8/ok4/ok2` prints what the live table admits, so the claim is a
// measurement in every run rather than a comment — but on this game's tables they are false and the
// P8/P4 arms stay scalar. **The widening that pays is item 2, and it is not contiguity.**
//
// ⭐ WHAT DOES PAY. `PSMT4HL` and `PSMT4HH` are C32-RESIDENT: they index `PageTableC32`, and
// G477 already PROVED (and verifies from the live table) that an 8-aligned group there lands on
// word offsets `row[g] + {0,1,4,5,8,9,12,13}`. Their byte address is `pa*4 + 3`, i.e. the TOP BYTE
// of each of those words, so eight texels are the top bytes of four adjacent word PAIRS at a
// 16-byte stride from one base `B = (pageBase + row[g]) * 4`:
//
//     B+3  B+7 | B+19 B+23 | B+35 B+39 | B+51 B+55
//
// Four `movq` loads, two 64-bit unpacks, two `psrld 24`, one `packssdw`, one `packuswb`, one mask
// and one `movq` store — **~12 SSE2 ops for 8 texels**, against ~7 scalar ops PER texel. The
// highest byte the group form touches is `B+55`, which is EXACTLY the highest byte the eight
// reference reads touch (`(pageBase + row[g] + 13) * 4 + 3`), so it is not an over-read and cannot
// fault where the reference would not.
//
// ⛔ SSE2 ONLY, deliberately. `_mm_shuffle_epi8` would collapse the extraction to one instruction
// but needs SSSE3 — the same call G491 made in `g491_readback_format.inc`, kept for one baseline.
//
// ⭐ THE THIRD LEVER, and it is free on every paletted texel: `g650PalTexel` applies
// `& (MEMORY_SIZE - 1)` per texel. That mask is the IDENTITY whenever the slice's largest byte
// address is in range, which is a function of `pageBase` and the row's largest table entry — both
// loop-invariant. So the arm is selected once per page slice (G645/G650's hoisting law) and the
// no-mask body drops one `and` from every texel of all four paletted formats.
//
// EXACTNESS. Unchanged in kind from G650: every arm's byte address is the same expression
// `PixelStorageTraits<psm>::Read` computes, every predicate is VERIFIED against the live page
// table at first use rather than assumed, and `DC2_G650_ROWPAL_VERIFY=1` still re-reads every
// produced texel through `Read` itself. Rollback `DC2_G650_NO_ROWPAL=1`; the widened arms have
// their own rollback `DC2_G651_NO_ROWPAL2=1`, which restores exactly the G650 bodies.
// ==============================================================================================

static const bool g_g651RowPal2On = (std::getenv("DC2_G651_NO_ROWPAL2") == nullptr);
static const bool g_g651RowPalStat = (std::getenv("DC2_G651_ROWPAL_STAT") != nullptr);
std::atomic<std::uint64_t> g_g651PalGrp8{0};
std::atomic<std::uint64_t> g_g651PalGrp4{0};
std::atomic<std::uint64_t> g_g651PalGrp2{0};
std::atomic<std::uint64_t> g_g651PalWord8{0};
std::atomic<std::uint64_t> g_g651PalScalar{0};
std::atomic<std::uint64_t> g_g651PalNoMask{0};
std::atomic<std::uint64_t> g_g651PalRowsOk8{0};
std::atomic<std::uint64_t> g_g651PalRowsOk4{0};
std::atomic<std::uint64_t> g_g651PalRowsOk2{0};

// Aligned-group contiguity at THREE widths, verified from the live table exactly as G477 does for
// CT32. `ok8 ⟹ ok4 ⟹ ok2` by construction (an 8-aligned contiguous run contains contiguous
// 4- and 2-aligned sub-runs), so the reader may test them in descending order and stop at the
// first that holds.
template <typename TableT>
struct G650PalGroupTable
{
    u8 ok[32][128]{};  // width 8
    u8 ok4[32][128]{};
    u8 ok2[32][128]{};
    u16 rowMax[32][128]{};

    G650PalGroupTable(const TableT &table, u32 pageW, u32 pageH)
    {
        for (u32 b = 0u; b < 32u; ++b)
        {
            for (u32 r = 0u; r < pageH && r < 128u; ++r)
            {
                const auto &row = table[b][r];
                u16 mx = 0u;
                for (u32 x = 0u; x < pageW; ++x)
                    if (row[x] > mx)
                        mx = row[x];
                rowMax[b][r] = mx;

                bool good8 = true, good4 = true, good2 = true;
                for (u32 g = 0u; good8 && g + 8u <= pageW; g += 8u)
                    for (u32 k = 1u; k < 8u; ++k)
                        if (row[g + k] != row[g] + k)
                        {
                            good8 = false;
                            break;
                        }
                for (u32 g = 0u; good4 && g + 4u <= pageW; g += 4u)
                    for (u32 k = 1u; k < 4u; ++k)
                        if (row[g + k] != row[g] + k)
                        {
                            good4 = false;
                            break;
                        }
                for (u32 g = 0u; good2 && g + 2u <= pageW; g += 2u)
                    if (row[g + 1u] != row[g] + 1u)
                        good2 = false;

                ok[b][r] = good8 ? 1u : 0u;
                ok4[b][r] = good4 ? 1u : 0u;
                ok2[b][r] = good2 ? 1u : 0u;
            }
        }
    }
};

template <PixelStorageMode psmV, typename TableT>
static bool g650PalGroupOk(const TableT &table, u32 block, u32 rowIndex, u32 pageW, u32 pageH,
                           u32 &rowMaxOut)
{
    static const G650PalGroupTable<TableT> built(table, pageW, pageH);
    rowMaxOut = built.rowMax[block][rowIndex];
    return built.ok[block][rowIndex] != 0u;
}

// The widened form: returns the largest verified contiguous group width (8, 4, 2) or 0.
template <PixelStorageMode psmV, typename TableT>
static u32 g651PalRunWidth(const TableT &table, u32 block, u32 rowIndex, u32 pageW, u32 pageH,
                           u32 &rowMaxOut)
{
    static const G650PalGroupTable<TableT> built(table, pageW, pageH);
    rowMaxOut = built.rowMax[block][rowIndex];
    if (built.ok[block][rowIndex])
        return 8u;
    if (built.ok4[block][rowIndex])
        return 4u;
    if (built.ok2[block][rowIndex])
        return 2u;
    return 0u;
}

// `[G651:rowpal]` — what the LIVE page tables admitted and which arm each texel took.
// `rowsOk8/4/2` count ROW SELECTIONS whose contiguous predicate held at that width; `grp*` and
// `word8` count TEXELS taken by each wide arm; `noMask` counts scalar texels that skipped the
// `& (MEMORY_SIZE - 1)`.
static void g651RowPalReport()
{
    std::fprintf(stderr,
                 "[G651:rowpal] rowsOk8=%llu rowsOk4=%llu rowsOk2=%llu | grp8=%llu grp4=%llu "
                 "grp2=%llu word8=%llu scalar=%llu noMask=%llu\n",
                 (unsigned long long)g_g651PalRowsOk8.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalRowsOk4.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalRowsOk2.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalGrp8.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalGrp4.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalGrp2.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalWord8.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalScalar.load(std::memory_order_relaxed),
                 (unsigned long long)g_g651PalNoMask.load(std::memory_order_relaxed));
}

// The per-texel decode, specialised per psm. `pa` is `PixelStorageTraits<psm>::Address(...)`.
template <PixelStorageMode psmV>
static inline u8 g650PalTexel(const u8 *data, u32 pa)
{
    constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 1u;
    if constexpr (psmV == P8)
    {
        return data[pa & kMask];
    }
    else if constexpr (psmV == P4)
    {
        // bits = pa * 4 ; byte = bits / 8 = pa >> 1 ; shift = bits % 8 = (pa & 1) * 4
        const u8 v = data[(pa >> 1u) & kMask];
        return static_cast<u8>((v >> ((pa & 1u) * 4u)) & 0x0Fu);
    }
    else if constexpr (psmV == P4HL)
    {
        // bits = pa * 32 + 24 ; byte = 4*pa + 3 ; shift = 0
        const u8 v = data[(pa * 4u + 3u) & kMask];
        return static_cast<u8>(v & 0x0Fu);
    }
    else // P4HH : bits = pa * 32 + 28 ; byte = 4*pa + 3 ; shift = 4
    {
        const u8 v = data[(pa * 4u + 3u) & kMask];
        return static_cast<u8>((v >> 4u) & 0x0Fu);
    }
}

// ⭐ G651 — the same per-texel decode with the `& (MEMORY_SIZE - 1)` mask REMOVED. Only ever
// reached under `g651PalSliceNoMask<psm>()`, which proves the mask is the identity for the whole
// page slice from the slice's `pageBase` and the row's largest table entry.
template <PixelStorageMode psmV>
static inline u8 g651PalTexelNoMask(const u8 *data, u32 pa)
{
    if constexpr (psmV == P8)
        return data[pa];
    else if constexpr (psmV == P4)
        return static_cast<u8>((data[pa >> 1u] >> ((pa & 1u) * 4u)) & 0x0Fu);
    else if constexpr (psmV == P4HL)
        return static_cast<u8>(data[pa * 4u + 3u] & 0x0Fu);
    else // P4HH
        return static_cast<u8>((data[pa * 4u + 3u] >> 4u) & 0x0Fu);
}

// The mask-identity proof, per psm, for one page slice. `pa` never exceeds `pageBase + rowMax`,
// so it is enough that the LARGEST byte address that `pa` can produce stays inside VRAM. Computed
// in 64 bits so the `pa * 4` form cannot wrap before the comparison.
template <PixelStorageMode psmV>
static inline bool g651PalSliceNoMask(u32 pageBase, u32 rowMax)
{
    const std::uint64_t paMax = static_cast<std::uint64_t>(pageBase) + rowMax;
    if constexpr (psmV == P8)
        return paMax < static_cast<std::uint64_t>(MEMORY_SIZE);
    else if constexpr (psmV == P4)
        return (paMax >> 1u) < static_cast<std::uint64_t>(MEMORY_SIZE);
    else // P4HL / P4HH : byte = pa * 4 + 3
        return paMax * 4ull + 3ull < static_cast<std::uint64_t>(MEMORY_SIZE);
}

// ⭐ G651 — eight P4HL/P4HH texels from four `movq` loads (SSE2 only; see the header note).
// Precondition (all verified by the caller): the G477 C32 word-group pattern holds for this
// (block, page row), the slice is mask-safe, and `idx + j` is 8-aligned.
template <PixelStorageMode psmV>
static inline void g651PalWordGroup8(const u8 *data, u32 base, u8 *out)
{
    const __m128i a = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(data + base));
    const __m128i b = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(data + base + 16u));
    const __m128i c = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(data + base + 32u));
    const __m128i d = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(data + base + 48u));
    // dwords in texel order: w0 w1 w4 w5 | w8 w9 w12 w13
    const __m128i lo = _mm_unpacklo_epi64(a, b);
    const __m128i hi = _mm_unpacklo_epi64(c, d);
    // byte 3 of each dword is the texel's storage byte (`pa * 4 + 3`)
    const __m128i t0 = _mm_srli_epi32(lo, 24);
    const __m128i t1 = _mm_srli_epi32(hi, 24);
    // values are 0..255, so neither saturating pack can clamp
    const __m128i packed = _mm_packus_epi16(_mm_packs_epi32(t0, t1), _mm_setzero_si128());
    const __m128i nib = _mm_set1_epi8(0x0F);
    const __m128i r = (psmV == P4HL) ? _mm_and_si128(packed, nib)
                                     : _mm_and_si128(_mm_srli_epi16(packed, 4), nib);
    _mm_storel_epi64(reinterpret_cast<__m128i *>(out), r);
}

template <PixelStorageMode psmV, typename TableT>
static inline void g650ReadRowPal(const TableT &table, u8 *data, u32 bp, u32 bw, u32 y, u32 w,
                                  u8 *out)
{
    using T = PixelStorageTraits<psmV>;
    if (!g_g650RowPalOn)
    {
        g512ReadRowImpl<psmV>(table, data, bp, bw, y, w, out);
        return;
    }

    constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 1u;
    constexpr u32 kPageW = static_cast<u32>(T::PageExtent().x);
    constexpr u32 kPageH = static_cast<u32>(T::PageExtent().y);
    constexpr u32 kPagePixels = static_cast<u32>(T::PixelsPerPage());
    // P4HL/P4HH are C32-RESIDENT: eight texels land on words `row[g] + {0,1,4,5,8,9,12,13}`.
    constexpr bool kWordPsm = (psmV == P4HL || psmV == P4HH);

    const u32 blk = bp % 32u;
    const u32 pr = y % kPageH;
    const auto &row = table[blk][pr];
    const u32 basePage = (bp / 32u) + (y / kPageH) * bw;

    // ---- hoisted, once per row: what the LIVE page table admits ------------------------------
    u32 rowMax = 0u;
    u32 runW = 0u;      // largest verified CONTIGUOUS group width (8 / 4 / 2), 0 if none
    bool wordGrp = false; // the G477 C32 word-pair pattern (P4HL / P4HH only)
    if (g_g651RowPal2On)
    {
        runW = g651PalRunWidth<psmV>(table, blk, pr, kPageW, kPageH, rowMax);
        if constexpr (kWordPsm)
            wordGrp = g477C32GroupOk<C32>(table, blk, pr);
        if (g_g651RowPalStat)
        {
            if (runW >= 8u) g_g651PalRowsOk8.fetch_add(1u, std::memory_order_relaxed);
            if (runW >= 4u) g_g651PalRowsOk4.fetch_add(1u, std::memory_order_relaxed);
            if (runW >= 2u) g_g651PalRowsOk2.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    else if constexpr (psmV == P8)
    {
        // G651 rollback arm: exactly the G650 predicate (8-wide contiguous, P8 only).
        runW = g650PalGroupOk<psmV>(table, blk, pr, kPageW, kPageH, rowMax) ? 8u : 0u;
    }

    u32 i = 0u;
    while (i < w)
    {
        const u32 pageBase = (basePage + (i / kPageW)) * kPagePixels;
        const u32 idx = i % kPageW;
        u32 lim = kPageW - idx;
        if (lim > w - i)
            lim = w - i;

        // The mask is the identity across the whole slice, or it is not — decided ONCE, here.
        const bool noMask = g_g651RowPal2On && g651PalSliceNoMask<psmV>(pageBase, rowMax);

        u32 j = 0u;

        if constexpr (psmV == P8)
        {
            // Byte-wise contiguous runs. `rowMax + width` must stay inside VRAM for the whole run.
            const bool safe8 =
                (static_cast<std::uint64_t>(pageBase) + rowMax + 8ull <= kMask + 1ull);
            if (runW >= 8u && safe8 && ((idx & 7u) == 0u))
            {
                while (j + 8u <= lim)
                {
                    std::memcpy(out + i + j, data + pageBase + row[idx + j], 8u);
                    j += 8u;
                }
                if (g_g651RowPalStat && j != 0u)
                    g_g651PalGrp8.fetch_add(j, std::memory_order_relaxed);
            }
            else if (runW == 4u && noMask && ((idx & 3u) == 0u))
            {
                while (j + 4u <= lim)
                {
                    std::memcpy(out + i + j, data + pageBase + row[idx + j], 4u);
                    j += 4u;
                }
                if (g_g651RowPalStat && j != 0u)
                    g_g651PalGrp4.fetch_add(j, std::memory_order_relaxed);
            }
            else if (runW == 2u && noMask && ((idx & 1u) == 0u))
            {
                while (j + 2u <= lim)
                {
                    std::memcpy(out + i + j, data + pageBase + row[idx + j], 2u);
                    j += 2u;
                }
                if (g_g651RowPalStat && j != 0u)
                    g_g651PalGrp2.fetch_add(j, std::memory_order_relaxed);
            }
        }
        else if constexpr (kWordPsm)
        {
            // ⭐ The arm that pays: 51% of `dungeon1`'s decoded texels are PSMT4HH.
            // `noMask` already proves `(pageBase + rowMax) * 4 + 3 < MEMORY_SIZE`, and the group's
            // highest byte is `(pageBase + row[g] + 13) * 4 + 3 <= (pageBase + rowMax) * 4 + 3`
            // because `row[g] + 13 == row[g + 7]` is itself an entry of this row.
            if (wordGrp && noMask && ((idx & 7u) == 0u))
            {
                while (j + 8u <= lim)
                {
                    g651PalWordGroup8<psmV>(data, (pageBase + row[idx + j]) * 4u, out + i + j);
                    j += 8u;
                }
                if (g_g651RowPalStat && j != 0u)
                    g_g651PalWord8.fetch_add(j, std::memory_order_relaxed);
            }
        }

        // ---- the residue: one arm, selected outside the loop (G645/G650 hoisting law) ---------
        if (j < lim)
        {
            if (g_g651RowPalStat)
            {
                g_g651PalScalar.fetch_add(lim - j, std::memory_order_relaxed);
                if (noMask)
                    g_g651PalNoMask.fetch_add(lim - j, std::memory_order_relaxed);
            }
            if (noMask)
            {
                for (; j < lim; ++j)
                    out[i + j] = g651PalTexelNoMask<psmV>(data, pageBase + row[idx + j]);
            }
            else
            {
                for (; j < lim; ++j)
                    out[i + j] = g650PalTexel<psmV>(data, pageBase + row[idx + j]);
            }
        }
        i += lim;
    }

    if (g_g650RowPalVerify)
    {
        for (u32 x = 0u; x < w; ++x)
        {
            const u8 ref = static_cast<u8>(T::Read(table, data, bp, bw, x, y));
            g_g650RowPalN.fetch_add(1u, std::memory_order_relaxed);
            if (ref != out[x])
            {
                const uint64_t bad = g_g650RowPalBad.fetch_add(1u, std::memory_order_relaxed) + 1ull;
                if (bad <= 16ull)
                    std::fprintf(stderr,
                                 "[G650:rowpal-BAD] psm=%d bp=0x%x bw=%u x=%u y=%u got=0x%02x ref=0x%02x\n",
                                 static_cast<int>(psmV), bp, bw, x, y, out[x], ref);
            }
        }
        const uint64_t n = g_g650RowPalN.load(std::memory_order_relaxed);
        if (n != 0ull && (n % 20000000ull) < w)
        {
            // ⚠️ G651: `grouped` is now ALWAYS 0 and is SUPERSEDED by `[G651:rowpal]`. The G650
            // counter was fed unconditionally (an atomic RMW per page slice on the shipped path);
            // the widened arms are counted only under `DC2_G651_ROWPAL_STAT=1`, so the shipped path
            // no longer pays for a census. Read `[G651:rowpal] grp8/grp4/grp2/word8/scalar`.
            std::fprintf(stderr, "[G650:rowpal] checks=%llu bad=%llu grouped=%llu\n",
                         (unsigned long long)n,
                         (unsigned long long)g_g650RowPalBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g650RowPalGrouped.load(std::memory_order_relaxed));
            g651RowPalReport();
        }
    }
    else if (g_g651RowPalStat)
    {
        // Behaviour-pure census cadence: one line per ~20 M texels of the widened arms.
        const uint64_t n = g_g651PalScalar.load(std::memory_order_relaxed) +
                           g_g651PalWord8.load(std::memory_order_relaxed) +
                           g_g651PalGrp8.load(std::memory_order_relaxed);
        if (n != 0ull && (n % 20000000ull) < w)
            g651RowPalReport();
    }
}

void G512ReadRowP8(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g650ReadRowPal<P8>(PageTableP8, data, bp, bw, y, w, out);
}

void G512ReadRowP4(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g650ReadRowPal<P4>(PageTableP4, data, bp, bw, y, w, out);
}

// P4HL/P4HH live in the C32 page layout and differ only in BitOffset (24 / 28) — same as the
// per-texel ReadP4HL/ReadP4HH above.
void G512ReadRowP4HL(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g650ReadRowPal<P4HL>(PageTableC32, data, bp, bw, y, w, out);
}

void G512ReadRowP4HH(u8 *data, u32 bp, u32 bw, u32 y, u32 w, u8 *out)
{
    g650ReadRowPal<P4HH>(PageTableC32, data, bp, bw, y, w, out);
}
