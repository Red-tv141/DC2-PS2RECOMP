// ===== G424: horizontal-run IMAGE upload writers (exact by construction) =====
//
// `GSMem::WriteCT32` and friends live in this translation unit; every caller of theirs lives in
// another one and the Release build uses `/O2 /Ob2` with NO `/GL`/LTCG. So the host-to-local IMAGE
// upload path in `GS::processImageData` paid one real cross-TU CALL per PIXEL — about 930k calls
// per frame on lean MAP-0 (3.7 MiB/f of PSMCT32 at 952 MiB/s, plus PSMT4 at 102 MiB/s). The
// G313 census placed the whole serial writer at ~5.1 ms/f on the GS worker, which G424's premise
// run measured as the frame pole.
//
// These helpers move the per-pixel loop INTO this TU. Each one calls exactly the same
// `PixelStorageTraits<psm>::Write(...)` with exactly the same arguments in exactly the same order
// as the legacy per-pixel caller did; nothing about swizzle addressing, masking, or the
// read-modify-write rules changes. The only difference is the call boundary, which lets MSVC
// inline the traits and hoist the page/block/row invariants that are constant for a run of
// consecutive x at a fixed y.
//
// Rollback: the caller's `DC2_G424_NO_FAST_IMAGE=1` restores the per-pixel call path.

namespace
{
    // One horizontal run: pixels [x, x+count) at row y. `Load` produces the packed value for
    // pixel i exactly as the legacy caller computed it.
    template <PixelStorageMode psm, typename TableT, typename LoadFn>
    inline void G424RunWrite(const TableT &table, u8 *data, u32 bp, u32 bw,
                             u32 x, u32 y, u32 count, LoadFn load)
    {
        using Traits = PixelStorageTraits<psm>;
        for (u32 i = 0u; i < count; ++i)
            Traits::Write(table, data, bp, bw, x + i, y,
                          static_cast<typename Traits::PackedT>(load(i)));
    }

}

// G477: swizzle-row hoisted forms of the two run writers this route actually spends time in.
#include "ps2_gs_memory_parts/g477_fast_run_writers.inc"

// --- 32-bit colour / depth ---------------------------------------------------------------------

void WriteRunCT32(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    if (g477CensusOn())
        g477NoteRun(0u, count);
    if (g477UseFast())
    {
        if (g477VerifyOn())
            g477RunWrite32<C32, true>(PageTableC32, data, bp, bw, x, y, src, count);
        else
            g477RunWrite32<C32, false>(PageTableC32, data, bp, bw, x, y, src, count);
        return;
    }
    G424RunWrite<C32>(PageTableC32, data, bp, bw, x, y, count, [src](u32 i) {
        u32 v;
        std::memcpy(&v, src + i * 4u, 4u);
        return v;
    });
}

void WriteRunZ32(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    if (g477UseFast())
    {
        if (g477VerifyOn())
            g477RunWrite32<Z32, true>(PageTableZ32, data, bp, bw, x, y, src, count);
        else
            g477RunWrite32<Z32, false>(PageTableZ32, data, bp, bw, x, y, src, count);
        return;
    }
    G424RunWrite<Z32>(PageTableZ32, data, bp, bw, x, y, count, [src](u32 i) {
        u32 v;
        std::memcpy(&v, src + i * 4u, 4u);
        return v;
    });
}

// CT24/Z24 share the 32-bit page layout. The legacy writer routed both through WriteCT32 and kept
// the pre-F62 quirk of storing 0x80 in the unused upper byte — reproduced verbatim here.
void WriteRunCT24(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<C32>(PageTableC32, data, bp, bw, x, y, count, [src](u32 i) {
        const u8 *p = src + i * 3u;
        return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u) |
               (static_cast<u32>(p[2]) << 16u) | (0x80u << 24u);
    });
}

// --- 16-bit colour / depth ---------------------------------------------------------------------

void WriteRunCT16(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<C16>(PageTableC16, data, bp, bw, x, y, count, [src](u32 i) {
        u16 v;
        std::memcpy(&v, src + i * 2u, 2u);
        return v;
    });
}

void WriteRunCT16S(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<C16S>(PageTableC16S, data, bp, bw, x, y, count, [src](u32 i) {
        u16 v;
        std::memcpy(&v, src + i * 2u, 2u);
        return v;
    });
}

void WriteRunZ16(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<Z16>(PageTableZ16, data, bp, bw, x, y, count, [src](u32 i) {
        u16 v;
        std::memcpy(&v, src + i * 2u, 2u);
        return v;
    });
}

void WriteRunZ16S(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<Z16S>(PageTableZ16S, data, bp, bw, x, y, count, [src](u32 i) {
        u16 v;
        std::memcpy(&v, src + i * 2u, 2u);
        return v;
    });
}

// --- paletted ----------------------------------------------------------------------------------

void WriteRunP8(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<P8>(PageTableP8, data, bp, bw, x, y, count,
                     [src](u32 i) { return static_cast<u32>(src[i]); });
}

// T8H stores the index in the upper byte of a 32-bit word (BitOffset 24 on the CT32 page table).
void WriteRunP8H(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    G424RunWrite<P8H>(PageTableC32, data, bp, bw, x, y, count,
                      [src](u32 i) { return static_cast<u32>(src[i]); });
}

// PSMT4: `nibble0` is the index of the first source nibble within `src`. Nibble n lives in
// `src[n / 2]`, low half first — the order the legacy byte loop consumed them.
void WriteRunP4(u8 *data, u32 bp, u32 bw, u32 x, u32 y,
                const u8 *src, u32 nibble0, u32 count)
{
    if (g477CensusOn())
        g477NoteRun(1u, count);
    if (g477UseFast())
    {
        if (g477VerifyOn())
            g477RunWriteP4<true>(data, bp, bw, x, y, src, nibble0, count);
        else
            g477RunWriteP4<false>(data, bp, bw, x, y, src, nibble0, count);
        return;
    }
    G424RunWrite<P4>(PageTableP4, data, bp, bw, x, y, count, [src, nibble0](u32 i) {
        const u32 n = nibble0 + i;
        return static_cast<u32>((src[n >> 1u] >> ((n & 1u) * 4u)) & 0x0Fu);
    });
}
