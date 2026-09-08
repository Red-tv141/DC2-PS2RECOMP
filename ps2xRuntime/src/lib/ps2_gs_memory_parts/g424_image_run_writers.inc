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

    // G706: PCSX2-class complete CT32/Z32 block writer. A GS 32-bit block is an 8x8 tile whose
    // 64 words occupy one contiguous 256-byte span in VRAM. Adjacent pairs of source rows map as
    //
    //   a0 a1 b0 b1 | a2 a3 b2 b3 | a4 a5 b4 b5 | a6 a7 b6 b7
    //
    // so four SSE2 64-bit interleaves write two complete rows. Four row pairs write the tile in
    // 16 wide stores, versus G477's 32 eight-byte stores plus eight page-row resolutions. This is
    // the same transform used by PCSX2's GSBlock::WriteBlock32.
    inline void g706SwizzleBlock32(u8 *dst, const u8 *src, u32 srcPitch)
    {
        for (u32 pair = 0u; pair < 4u; ++pair)
        {
            const u8 *s0 = src + pair * 2u * srcPitch;
            const u8 *s1 = s0 + srcPitch;
            const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s0));
            const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s0 + 16u));
            const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s1));
            const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s1 + 16u));
            u8 *d = dst + pair * 64u;
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d),
                             _mm_unpacklo_epi64(a0, b0));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 16u),
                             _mm_unpackhi_epi64(a0, b0));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32u),
                             _mm_unpacklo_epi64(a1, b1));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48u),
                             _mm_unpackhi_epi64(a1, b1));
        }
    }

}

// G477: swizzle-row hoisted forms of the two run writers this route actually spends time in.
#include "ps2_gs_memory_parts/g477_fast_run_writers.inc"

namespace
{
    template <PixelStorageMode psm, typename TableT>
    inline void g706WriteBlockRect32(const TableT &table, u8 *data, u32 bp, u32 bw,
                                     u32 x, u32 y, const u8 *src, u32 srcPitch,
                                     u32 blocksWide, u32 blocksHigh)
    {
        using Traits = PixelStorageTraits<psm>;
        constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 4u;
        constexpr u32 kBlockBytes = 8u * 8u * 4u;

        for (u32 by = 0u; by < blocksHigh; ++by)
        {
            const u32 py = y + by * 8u;
            const u8 *srcRow = src + by * 8u * srcPitch;
            for (u32 bx = 0u; bx < blocksWide; ++bx)
            {
                const u32 px = x + bx * 8u;
                const u32 byteBase = (Traits::Address(table, bp, bw, px, py) * 4u) & kMask;
                const u8 *srcBlock = srcRow + bx * 8u * 4u;

                // PixelStorageTraits masks each destination word independently. A block straddling
                // the 4 MiB ring boundary therefore is not one linear span; retain G477's exact row
                // writer for that rare tile.
                if (byteBase > static_cast<u32>(MEMORY_SIZE) - kBlockBytes)
                {
                    for (u32 row = 0u; row < 8u; ++row)
                        g477RunWrite32<psm, false>(table, data, bp, bw, px, py + row,
                                                  srcBlock + row * srcPitch, 8u);
                    continue;
                }
                g706SwizzleBlock32(data + byteBase, srcBlock, srcPitch);
            }
        }
    }
}

void WriteBlockRectCT32(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src,
                        u32 srcPitch, u32 blocksWide, u32 blocksHigh)
{
    g706WriteBlockRect32<C32>(PageTableC32, data, bp, bw, x, y, src, srcPitch,
                              blocksWide, blocksHigh);
}

void WriteBlockRectZ32(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src,
                       u32 srcPitch, u32 blocksWide, u32 blocksHigh)
{
    g706WriteBlockRect32<Z32>(PageTableZ32, data, bp, bw, x, y, src, srcPitch,
                              blocksWide, blocksHigh);
}

// ===== G704 UPLOAD-REDUNDANCY CENSUS (compile-time, -DPS2X_G704_UPCENSUS=ON, behaviour-pure) =====
//
// THE QUESTION IT EXISTS TO ANSWER, priced before any lever is written (the G476 law).
//
// `[G511:texsplit]` on `dragon` puts ~40% of the front-end `tex` pass in the CONTENT-HASH tier:
// ~0.95 hashes per flush over **305-406 KB each**, i.e. roughly **15 MB/frame of guest VRAM
// re-read** for no purpose but to answer "did this texture actually change?". That tier exists
// only because the cheap tier above it missed: `g178GenSumRect` compares page GENERATIONS, and the
// guest bumps those by re-uploading the texture every frame — the G424/G477 headers say outright
// that DC2's re-uploads are "usually byte-identical".
//
// If they are, the generation bump is spurious for cache purposes and the whole hash tier is
// avoidable: an upload that writes the bytes already present need not invalidate anything.
//
// ⚠️ ONE COMPETING DESIGN IS ALREADY REFUTED BY ARITHMETIC, before either was built. Moving the
// work to a per-upload DIGEST loses: `[G313:seg]` counts ~118 host->local IMAGE calls per frame
// against ~47 hashes per frame, so digesting every upload does 2.5x the work of hashing every
// consumed texture. The surviving design is a COMPARE, because it is traffic-neutral at the write
// (it reads the destination it was about to overwrite) and pays only on uploads, not on consumers.
//
// So the number that decides it is: what SHARE of uploaded bytes are already present? This census
// measures exactly that, on the CT32 path (`[G313:psm] psm=0x00` is 3.4 GB of the run's 3.7 GB).
// It is a PROBE, not a shippable path — it adds a full destination read — hence compile-time
// exclusion (rule 12c: this is `ps2_gs_memory.cpp`, whose run writers move ~7 GB/s).
#if defined(PS2X_G704_UPCENSUS)
namespace
{
    std::atomic<unsigned long long> g_g704UpSame{0ull};     // PIXELS already equal
    std::atomic<unsigned long long> g_g704UpDiff{0ull};     // pixels that genuinely changed
    std::atomic<unsigned long long> g_g704UpRuns{0ull};     // runs inspected
    std::atomic<unsigned long long> g_g704UpRunsSame{0ull}; // runs identical end to end

    // One run's verdict. Counted in PIXELS (4 B each on this path) plus a whole-run flag, because
    // the lever the census is pricing can only skip an invalidation when the WHOLE run is
    // unchanged — a per-byte share alone would overstate it (the G508 law: a price is COUNT x COST,
    // and the count here is runs, not bytes).
    inline void g704NoteUploadCounts(u32 same, u32 diff)
    {
        if (same != 0u)
            g_g704UpSame.fetch_add(same, std::memory_order_relaxed);
        if (diff != 0u)
            g_g704UpDiff.fetch_add(diff, std::memory_order_relaxed);
        else if (same != 0u)
            g_g704UpRunsSame.fetch_add(1ull, std::memory_order_relaxed);
        const unsigned long long n =
            g_g704UpRuns.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if ((n % 2000000ull) == 0ull)
        {
            const double s = static_cast<double>(g_g704UpSame.load(std::memory_order_relaxed));
            const double d = static_cast<double>(g_g704UpDiff.load(std::memory_order_relaxed));
            const double rs = static_cast<double>(g_g704UpRunsSame.load(std::memory_order_relaxed));
            std::fprintf(stderr,
                         "[G704:upcensus] runs=%llu sameRuns=%.0f (%.2f%%) "
                         "samePx=%.0f diffPx=%.0f samePxPct=%.2f%%\n",
                         n, rs, 100.0 * rs / static_cast<double>(n), s, d,
                         (s + d) > 0.0 ? 100.0 * s / (s + d) : 0.0);
        }
    }

    // The compare-mode mirror of `g477RunWrite32`'s scalar tail. It resolves the destination byte
    // address with the SAME expression the writer uses — `((pageBase + row[idx+j]) * 32) / 8`
    // masked to VRAM — so the bytes compared are exactly the bytes that are about to be written.
    // Deliberately NOT the pair/group fast form: this is a probe, correctness of attribution beats
    // its own speed, and the scalar walk covers every layout the fast form falls back from.
    template <typename TableT>
    inline void g704CensusRunCT32Impl(const TableT &table, const u8 *data, u32 bp, u32 bw,
                                      u32 x, u32 y, const u8 *src, u32 count)
    {
        constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 4u;
        constexpr u32 kPageW = 64u;
        constexpr u32 kPageH = 32u;
        constexpr u32 kPagePixels = kPageW * kPageH;
        const auto &row = table[bp % 32u][y % kPageH];
        const u32 basePage = (bp / 32u) + (y / kPageH) * bw;
        u32 same = 0u, diff = 0u;
        u32 i = 0u;
        while (i < count)
        {
            const u32 xi = x + i;
            const u32 pageBase = (basePage + (xi / kPageW)) * kPagePixels;
            const u32 idx = xi & (kPageW - 1u);
            u32 lim = kPageW - idx;
            if (lim > count - i)
                lim = count - i;
            for (u32 j = 0u; j < lim; ++j)
            {
                const u32 b = (((pageBase + row[idx + j]) * 32u) / 8u) & kMask;
                if (std::memcmp(data + b, src + (i + j) * 4u, 4u) == 0)
                    ++same;
                else
                    ++diff;
            }
            i += lim;
        }
        g704NoteUploadCounts(same, diff);
    }
}
#define G704_CENSUS_RUN_CT32(tbl, data, bp, bw, x, y, src, count) \
    g704CensusRunCT32Impl((tbl), (data), (bp), (bw), (x), (y), (src), (count))
#else
#define G704_CENSUS_RUN_CT32(tbl, data, bp, bw, x, y, src, count) ((void)0)
#endif

// --- 32-bit colour / depth ---------------------------------------------------------------------

void WriteRunCT32(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src, u32 count)
{
    // G704: runs are contiguous in the SOURCE but swizzled in the DESTINATION, so the comparison
    // has to follow the same page/column walk the writer does — which is why this mirrors
    // `g477RunWrite32`'s address expression rather than inventing a second one. Compiles to
    // nothing without -DPS2X_G704_UPCENSUS=ON.
    G704_CENSUS_RUN_CT32(PageTableC32, data, bp, bw, x, y, src, count);
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
