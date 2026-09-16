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
    // ===== G724: IS THE UPLOAD WRITING BYTES THAT ARE ALREADY THERE? ============================
    //
    // G704's upload-redundancy note states the premise and the arithmetic: DC2 re-uploads the same
    // textures every frame and the G424/G477 headers call them "usually byte-identical", which is
    // why `g178GenSumRect`'s page generations are bumped spuriously and the content-hash tier of
    // `tex` exists at all. It also settled the DESIGN: a per-upload digest loses to a COMPARE,
    // because a compare is traffic-neutral at the write (it reads the destination it was about to
    // overwrite) and pays only on uploads rather than on every consumer. It was never built,
    // because the census that decides it was compile-time only.
    //
    // ⭐ WHAT MAKES IT WORTH BUILDING NOW: under G721 the whole upload path runs on the EXECUTOR,
    // which is the pole, while the PARSE thread sits blocked ~10 ms/f. Anything this compare can
    // prove is redundant is work deleted from the pole. And 99.99% of uploaded bytes now go through
    // the block path (`[G706:block] share=99.99%`), so ONE place sees essentially all of them.
    //
    // Two arms, deliberately separate:
    //   DC2_G724_UPSTAT=1  census only - compare, then store unconditionally. BEHAVIOUR-PURE, so
    //                      the share it reports is the share the shipped path would have seen.
    //   DC2_G724_UPSKIP=1  compare, and skip the store when the block is byte-identical. Skipping
    //                      a store of bytes already present cannot change VRAM; what it changes is
    //                      that the destination line is not dirtied and not written back.
    //
    // ⛔ This does NOT yet skip `g149BumpVramGen` or the g178 write-rect note - those run BEFORE
    // the writer and would need the compare hoisted ahead of them. That is the phase-2 lever this
    // census exists to price, and it is the one with the flush-count consequence.
    bool g724UpStatOn()
    {
        static const bool v = [] {
            const char *e = std::getenv("DC2_G724_UPSTAT");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
        }();
        return v;
    }
    bool g724UpSkipOn()
    {
        static const bool v = [] {
            const char *e = std::getenv("DC2_G724_UPSKIP");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
        }();
        return v;
    }

    // Plain counters: under the pipeline every upload runs on the executor, and outside it on the
    // GS thread. A census race would cost a report digit, never behaviour.
    uint64_t g_g724Blocks = 0u, g_g724Same = 0u, g_g724Calls = 0u;

    inline void g724Report()
    {
        if ((++g_g724Calls & 8191u) != 0u)
            return;
        std::fprintf(stderr, "[G724:upskip] calls=%llu blocks=%llu identical=%llu (%.2f%%)\n",
                     static_cast<unsigned long long>(g_g724Calls),
                     static_cast<unsigned long long>(g_g724Blocks),
                     static_cast<unsigned long long>(g_g724Same),
                     g_g724Blocks ? 100.0 * static_cast<double>(g_g724Same) /
                                        static_cast<double>(g_g724Blocks)
                                  : 0.0);
        std::fflush(stderr);
    }

    // Build the swizzled 256-byte block into a local buffer with the identical shuffle sequence
    // g706SwizzleBlock32 uses, compare it against the destination, and store only when it differs
    // (or always, under the census-only arm). Returns true when the destination already held it.
    inline bool g724SwizzleBlock32Cmp(u8 *dst, const u8 *src, u32 srcPitch, bool skipStore)
    {
        alignas(16) u8 staged[8u * 8u * 4u];
        for (u32 pair = 0u; pair < 4u; ++pair)
        {
            const u8 *s0 = src + pair * 2u * srcPitch;
            const u8 *s1 = s0 + srcPitch;
            const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s0));
            const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s0 + 16u));
            const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s1));
            const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s1 + 16u));
            u8 *d = staged + pair * 64u;
            _mm_store_si128(reinterpret_cast<__m128i *>(d), _mm_unpacklo_epi64(a0, b0));
            _mm_store_si128(reinterpret_cast<__m128i *>(d + 16u), _mm_unpackhi_epi64(a0, b0));
            _mm_store_si128(reinterpret_cast<__m128i *>(d + 32u), _mm_unpacklo_epi64(a1, b1));
            _mm_store_si128(reinterpret_cast<__m128i *>(d + 48u), _mm_unpackhi_epi64(a1, b1));
        }
        const bool same = std::memcmp(dst, staged, sizeof(staged)) == 0;
        if (!same || !skipStore)
            std::memcpy(dst, staged, sizeof(staged));
        return same;
    }

    template <PixelStorageMode psm, typename TableT>
    inline void g706WriteBlockRect32(const TableT &table, u8 *data, u32 bp, u32 bw,
                                     u32 x, u32 y, const u8 *src, u32 srcPitch,
                                     u32 blocksWide, u32 blocksHigh)
    {
        using Traits = PixelStorageTraits<psm>;
        constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 4u;
        constexpr u32 kBlockBytes = 8u * 8u * 4u;
        const bool g724Skip = g724UpSkipOn();
        const bool g724Cmp = g724Skip || g724UpStatOn();

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
                if (g724Cmp)
                {
                    ++g_g724Blocks;
                    if (g724SwizzleBlock32Cmp(data + byteBase, srcBlock, srcPitch, g724Skip))
                        ++g_g724Same;
                    continue;
                }
                g706SwizzleBlock32(data + byteBase, srcBlock, srcPitch);
            }
        }
        if (g724Cmp)
            g724Report();
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

// ==============================================================================================
// ⭐⭐⭐ G742 — THE COMPLETE-BLOCK PSMT4 IMAGE WRITER. G706'S MISSING SIBLING.
// ==============================================================================================
//
// THE DEFECT. G706 promoted the PCSX2-class complete-block IMAGE writer and wired it to **two of
// the four block-writable destination formats**: `g424_fast_image_upload.inc` reads
// `g706BlockFormat = dpsm == GS_PSM_CT32 || dpsm == GS_PSM_Z32`, and the `GS_PSM_T4` / `GS_PSM_T8`
// arms `return true` at the TOP of that function, before the block stage is ever reached. PSMT4
// therefore still goes through `WriteRunP4` — one read-modify-write of one NIBBLE per pixel, plus a
// page-row table lookup per pixel.
//
// WHAT IT COSTS, MEASURED (G742, `dungeon1`, 5,821 rendered frames, `DC2_G313_CENSUS=1`, shipping
// config, on the G713 EXECUTOR — the thread G740 §4d.2 measured saturated and G742 re-measured with
// only **2.4 ms/f of idle**, i.e. the pole):
//
//   [G313:psm] psm=0x00 calls=785816 bytes=12428850560 ms=1468.6 MiBps=8071 ... block=769442
//   [G313:psm] psm=0x14 calls=16372  bytes=1629880320  ms=4460.8 MiBps=348  ... block=16372
//
// | format | per frame | per PIXEL | shape |
// |---|---:|---:|---|
// | PSMCT32 (has the block writer) | 0.252 ms/f | **0.47 ns** | 98 % block-aligned |
// | **PSMT4 (does not)** | **0.766 ms/f** | **1.37 ns** | **100 % block-aligned** |
//
// ⇒ PSMT4 is **74 % of the whole IMAGE writer** on this route while carrying 12 % of its bytes, it
// costs **2.9x per pixel** what the block-written format costs, and **every single one of its 16,372
// transfers already satisfies the block geometry** the fast stage requires. The eligible population
// is 100 % and the fast path was never offered to it — the mission's "a fast path with zero use
// despite a huge eligible population", exactly.
//
// MECHANISM. A PSMT4 block is 32x16 pixels = 512 nibbles = **256 contiguous bytes** of VRAM, and the
// in-block permutation is the same for every block of every page (that is what a column table IS).
// So a block-aligned write needs no per-pixel addressing and no read-modify-write at all: gather the
// 512 source nibbles, then emit 256 whole bytes.
//
//   1. `g742P4Map()` derives the 512-entry permutation ONCE, **from `PageTableP4` itself** — not
//      from an assumed layout — and inverts it to `srcOfAddr[]`. Destination byte `b` holds nibble
//      addresses `2b` (low half) and `2b+1` (high half), so its two source pixels are
//      `srcOfAddr[2b]` and `srcOfAddr[2b+1]`.
//   2. It then VERIFIES block-invariance over all 32 blocks of a page before arming: if any block's
//      relative permutation differs from block 0's, `ok` stays false and every caller keeps the row
//      writer. A layout this code cannot prove is a layout it does not use.
//   3. Per block: 16 source rows x 16 contiguous bytes are unpacked to a 512-byte scratch (each
//      source byte is low nibble first — the order `WriteRunP4` consumes them), then 256 stores.
//
// EXACTNESS. Every destination nibble receives the value of the same source pixel the scalar path
// would have written there, because the mapping IS the scalar path's own table. The RMW disappears
// safely only because block geometry guarantees the transfer owns all 512 nibbles of the block —
// `g424_fast_image_upload.inc` checks that (x and rrw multiples of 32, y multiple of 16, >= 16 rows
// remaining) before calling, and any block that would straddle the 4 MiB VRAM ring falls back to the
// row writer, exactly as `g706WriteBlockRect32` does.
//
// Oracle: `DC2_G742_P4BLOCK_VERIFY=1` writes each block BOTH ways into a private shadow of the
// affected bytes and compares all 256. Rollback: `DC2_G742_NO_P4BLOCK=1` restores the row writer.
// ==============================================================================================
namespace
{
    struct G742P4BlockMap
    {
        bool ok = false;
        u16 srcOfAddr[512]{}; // nibble address within the block -> source pixel index (r*32 + c)
    };

    const G742P4BlockMap &g742P4Map()
    {
        static const G742P4BlockMap map = []() {
            G742P4BlockMap m;
            constexpr u32 kBW = 32u;  // block width in pixels
            constexpr u32 kBH = 16u;  // block height in pixels
            constexpr u32 kN = kBW * kBH;
            static_assert(kN == 512u, "G742: a PSMT4 block is 512 nibbles");

            // `InitPageLookupTable` writes
            //     table[block][y][x] = (block + BlockId(x, y)) * 512 + ColumnId(x, y)
            // so for block index 0 over the page's FIRST block (x < 32, y < 16, BlockId == 0) the
            // raw entry IS `ColumnId`, i.e. the offset from the block's 512-aligned base. That is
            // exactly the index the runtime derives as `Address(...) & 511`, so `perm` must be the
            // RAW value here — not a value rebased on entry [0][0].
            u32 perm[kN];
            bool seen[kN] = {};
            for (u32 r = 0u; r < kBH; ++r)
                for (u32 c = 0u; c < kBW; ++c)
                {
                    const u32 a = static_cast<u32>(PageTableP4[0][r][c]);
                    if (a >= kN || seen[a])
                        return m; // not a bijection over the block: leave ok=false
                    seen[a] = true;
                    perm[r * kBW + c] = a;
                }

            // Block-invariance: every one of the page's 32 blocks must carry the SAME relative
            // permutation, or a single table cannot describe them. `perm[0]` is subtracted because
            // the block's base is where the 512-aligned span starts, not where pixel (0,0) lands.
            for (u32 by = 0u; by * kBH < 128u; ++by)
                for (u32 bx = 0u; bx * kBW < 128u; ++bx)
                {
                    const u32 corner =
                        static_cast<u32>(PageTableP4[0][by * kBH][bx * kBW]);
                    if (corner < perm[0])
                        return m;
                    const u32 blockBase = corner - perm[0];
                    if ((blockBase % kN) != 0u)
                        return m; // a block base that is not 512-aligned breaks the whole scheme
                    for (u32 r = 0u; r < kBH; ++r)
                        for (u32 c = 0u; c < kBW; ++c)
                        {
                            const u32 got =
                                static_cast<u32>(PageTableP4[0][by * kBH + r][bx * kBW + c]);
                            if (got != blockBase + perm[r * kBW + c])
                                return m;
                        }
                }

            for (u32 i = 0u; i < kN; ++i)
                m.srcOfAddr[perm[i]] = static_cast<u16>(i);
            m.ok = true;
            return m;
        }();
        return map;
    }

    // ---- the PSMT8 sibling. Same derivation, one byte per pixel instead of one nibble. ----------
    //
    // ⭐ Rule G742-2 applied to its own finding: G706 stopped at two formats, so this phase
    // enumerates ALL of them. A PSMT8 block is 16x16 pixels = **256 pixels = 256 bytes**, one byte
    // each, so the destination write is a pure 256-entry PERMUTATION — no nibble packing at all.
    // `WriteRunP8` has no G477 fast path either (it goes straight to
    // `PixelStorageTraits<P8>::Write` per pixel), which is why the census reads it at 1.30 ns/pixel,
    // the same as PSMT4 and 2.8x PSMCT32.
    struct G742P8BlockMap
    {
        bool ok = false;
        u16 srcOfAddr[256]{}; // byte offset within the block -> source pixel index (r*16 + c)
    };

    const G742P8BlockMap &g742P8Map()
    {
        static const G742P8BlockMap map = []() {
            G742P8BlockMap m;
            constexpr u32 kBW = 16u;
            constexpr u32 kBH = 16u;
            constexpr u32 kN = kBW * kBH;
            static_assert(kN == 256u, "G742: a PSMT8 block is 256 bytes");
            u32 perm[kN];
            bool seen[kN] = {};
            for (u32 r = 0u; r < kBH; ++r)
                for (u32 c = 0u; c < kBW; ++c)
                {
                    const u32 a = static_cast<u32>(PageTableP8[0][r][c]);
                    if (a >= kN || seen[a])
                        return m;
                    seen[a] = true;
                    perm[r * kBW + c] = a;
                }
            // The PSMT8 page is 128x64, i.e. 8 blocks wide and 4 high.
            for (u32 by = 0u; by * kBH < 64u; ++by)
                for (u32 bx = 0u; bx * kBW < 128u; ++bx)
                {
                    const u32 corner = static_cast<u32>(PageTableP8[0][by * kBH][bx * kBW]);
                    if (corner < perm[0])
                        return m;
                    const u32 blockBase = corner - perm[0];
                    if ((blockBase % kN) != 0u)
                        return m;
                    for (u32 r = 0u; r < kBH; ++r)
                        for (u32 c = 0u; c < kBW; ++c)
                        {
                            const u32 got =
                                static_cast<u32>(PageTableP8[0][by * kBH + r][bx * kBW + c]);
                            if (got != blockBase + perm[r * kBW + c])
                                return m;
                        }
                }
            for (u32 i = 0u; i < kN; ++i)
                m.srcOfAddr[perm[i]] = static_cast<u16>(i);
            m.ok = true;
            return m;
        }();
        return map;
    }

    bool g742P4BlockVerifyOn()
    {
        static const bool v = std::getenv("DC2_G742_P4BLOCK_VERIFY") != nullptr;
        return v;
    }

    std::atomic<unsigned long long> g_g742P4Blocks{0ull};
    std::atomic<unsigned long long> g_g742P4VerifyChecks{0ull};
    std::atomic<unsigned long long> g_g742P4VerifyBad{0ull};

    // One block: 16 source rows of 16 contiguous bytes -> 256 contiguous destination bytes.
    inline void g742WriteBlockP4(u8 *dst, const u8 *src, u32 srcPitch,
                                 const G742P4BlockMap &map)
    {
        u8 blk[512];
        for (u32 r = 0u; r < 16u; ++r)
        {
            const u8 *s = src + static_cast<usz>(r) * srcPitch;
            u8 *o = blk + r * 32u;
            for (u32 k = 0u; k < 16u; ++k)
            {
                const u8 byte = s[k];
                o[k * 2u + 0u] = static_cast<u8>(byte & 0x0Fu);
                o[k * 2u + 1u] = static_cast<u8>(byte >> 4u);
            }
        }
        for (u32 b = 0u; b < 256u; ++b)
            dst[b] = static_cast<u8>(blk[map.srcOfAddr[b * 2u]] |
                                     (blk[map.srcOfAddr[b * 2u + 1u]] << 4u));
    }
}

bool WriteBlockRectP4Available()
{
    // `envFlagEnabled` does not exist in this translation unit (ps2_gs_memory.cpp includes only the
    // GS memory header), so read the rollback the way every other flag in this file does.
    static const bool v = (std::getenv("DC2_G742_NO_P4BLOCK") == nullptr) && g742P4Map().ok;
    return v;
}

// `src` addresses the stripe's first row at rect column 0; `srcPitch` is the rect's row length in
// BYTES (rrw/2, exact because block geometry forces rrw % 32 == 0). `x`/`y` are the destination
// coordinates of that same origin.
void WriteBlockRectP4(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src,
                      u32 srcPitch, u32 blocksWide, u32 blocksHigh)
{
    using Traits = PixelStorageTraits<P4>;
    // The scalar path masks EACH nibble's byte address with `MEMORY_SIZE - 1` (g477RunWriteP4's
    // `kMask`, PackedT = u8). `MEMORY_SIZE` is 4 MiB — a power of two — and a block's byte base is
    // 256-ALIGNED after masking (see below), so `(base & kMask) + d` for `d < 256` reproduces
    // `(base + d) & kMask` exactly and a block can never straddle the ring. That is why this writer
    // needs no ring fallback while `g706WriteBlockRect32` does: a CT32 block's base is only
    // 4-aligned in bytes, a P4 block's is 256-aligned.
    constexpr u32 kByteMask = static_cast<u32>(MEMORY_SIZE) - 1u;
    static_assert((static_cast<u32>(MEMORY_SIZE) & kByteMask) == 0u,
                  "G742: the byte mask assumes a power-of-two VRAM size");
    const G742P4BlockMap &map = g742P4Map();
    const bool verify = g742P4BlockVerifyOn();

    for (u32 by = 0u; by < blocksHigh; ++by)
    {
        const u32 py = y + by * 16u;
        const u8 *srcRow = src + static_cast<usz>(by) * 16u * srcPitch;
        for (u32 bx = 0u; bx < blocksWide; ++bx)
        {
            const u32 px = x + bx * 32u;
            // `Address` = page*16384 + (block + BlockId)*512 + ColumnId, and ColumnId is exactly
            // the low 9 bits (0..511). So masking them off yields the block's first nibble address
            // whichever pixel of the block is asked for, and it is 512-aligned by construction —
            // hence a 256-aligned, 256-byte contiguous destination span.
            const u32 blockBase = Traits::Address(PageTableP4, bp, bw, px, py) & ~511u;
            const u32 byteBase = (blockBase >> 1u) & kByteMask;
            const u8 *srcBlock = srcRow + bx * 16u;

            if (verify)
            {
                u8 shadow[256];
                std::memcpy(shadow, data + byteBase, 256u);
                for (u32 r = 0u; r < 16u; ++r)
                    g477RunWriteP4<false>(data, bp, bw, px, py + r,
                                          srcBlock + static_cast<usz>(r) * srcPitch, 0u, 32u);
                u8 reference[256];
                std::memcpy(reference, data + byteBase, 256u);
                std::memcpy(data + byteBase, shadow, 256u);
                g742WriteBlockP4(data + byteBase, srcBlock, srcPitch, map);
                g_g742P4VerifyChecks.fetch_add(1ull, std::memory_order_relaxed);
                if (std::memcmp(reference, data + byteBase, 256u) != 0)
                {
                    const unsigned long long bad =
                        g_g742P4VerifyBad.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
                    if (bad <= 8ull)
                        std::fprintf(stderr,
                                     "[G742:p4blk-BAD] n=%llu bp=%u bw=%u px=%u py=%u byteBase=%u\n",
                                     bad, bp, bw, px, py, byteBase);
                }
                if ((g_g742P4VerifyChecks.load(std::memory_order_relaxed) % 65536ull) == 0ull)
                    std::fprintf(stderr, "[G742:p4blk] checks=%llu bad=%llu\n",
                                 (unsigned long long)g_g742P4VerifyChecks.load(std::memory_order_relaxed),
                                 (unsigned long long)g_g742P4VerifyBad.load(std::memory_order_relaxed));
                continue;
            }

            g742WriteBlockP4(data + byteBase, srcBlock, srcPitch, map);
        }
    }
    g_g742P4Blocks.fetch_add(static_cast<unsigned long long>(blocksWide) * blocksHigh,
                             std::memory_order_relaxed);
}

unsigned long long WriteBlockRectP4Count()
{
    return g_g742P4Blocks.load(std::memory_order_relaxed);
}

bool WriteBlockRectP8Available()
{
    static const bool v = (std::getenv("DC2_G742_NO_P8BLOCK") == nullptr) && g742P8Map().ok;
    return v;
}

// PSMT8: `srcPitch` is the rect's row length in BYTES (== rrw, one byte per pixel).
void WriteBlockRectP8(u8 *data, u32 bp, u32 bw, u32 x, u32 y, const u8 *src,
                      u32 srcPitch, u32 blocksWide, u32 blocksHigh)
{
    using Traits = PixelStorageTraits<P8>;
    // P8's unpacked width is 8 bits, so the pixel address IS the byte address and the scalar path's
    // mask is `MEMORY_SIZE - 1`. A block base is 256-aligned, so the 256-byte span never straddles
    // the ring — the same argument as the P4 writer above.
    constexpr u32 kByteMask = static_cast<u32>(MEMORY_SIZE) - 1u;
    const G742P8BlockMap &map = g742P8Map();
    const bool verify = g742P4BlockVerifyOn(); // one oracle flag covers both block writers

    for (u32 by = 0u; by < blocksHigh; ++by)
    {
        const u32 py = y + by * 16u;
        const u8 *srcRow = src + static_cast<usz>(by) * 16u * srcPitch;
        for (u32 bx = 0u; bx < blocksWide; ++bx)
        {
            const u32 px = x + bx * 16u;
            const u32 byteBase =
                (Traits::Address(PageTableP8, bp, bw, px, py) & ~255u) & kByteMask;
            const u8 *srcBlock = srcRow + bx * 16u;

            if (verify)
            {
                u8 shadow[256];
                std::memcpy(shadow, data + byteBase, 256u);
                for (u32 r = 0u; r < 16u; ++r)
                    G424RunWrite<P8>(PageTableP8, data, bp, bw, px, py + r, 16u,
                                     [p = srcBlock + static_cast<usz>(r) * srcPitch](u32 i) {
                                         return static_cast<u32>(p[i]);
                                     });
                u8 reference[256];
                std::memcpy(reference, data + byteBase, 256u);
                std::memcpy(data + byteBase, shadow, 256u);
                for (u32 b = 0u; b < 256u; ++b)
                {
                    const u32 s = map.srcOfAddr[b];
                    data[byteBase + b] = srcBlock[(s >> 4u) * srcPitch + (s & 15u)];
                }
                g_g742P4VerifyChecks.fetch_add(1ull, std::memory_order_relaxed);
                if (std::memcmp(reference, data + byteBase, 256u) != 0)
                {
                    const unsigned long long bad =
                        g_g742P4VerifyBad.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
                    if (bad <= 8ull)
                        std::fprintf(stderr,
                                     "[G742:p8blk-BAD] n=%llu bp=%u bw=%u px=%u py=%u byteBase=%u\n",
                                     bad, bp, bw, px, py, byteBase);
                }
                continue;
            }

            u8 *const dst = data + byteBase;
            for (u32 b = 0u; b < 256u; ++b)
            {
                const u32 s = map.srcOfAddr[b];
                dst[b] = srcBlock[(s >> 4u) * srcPitch + (s & 15u)];
            }
        }
    }
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
