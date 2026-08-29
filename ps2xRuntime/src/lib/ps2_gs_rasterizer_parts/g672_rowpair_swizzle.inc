// ===== G672: the ROW-PAIR PSMCT32 swizzle kernel (exact by construction) =======================
//
// WHY THIS EXISTS — the arithmetic that named it (G672 P1, `dungeon1` steady tail).
//
// Three sites on the GS worker (the pole in 21 of 23 canonical windows) convert between guest
// PSMCT32 VRAM and a linear bottom-first RGBA8 host buffer, and all three run the SAME inner loop
// shape that G418/G419 introduced: one 32x64 page-local offset table lookup per PIXEL PAIR plus an
// 8-byte `memcpy`. Measured, censused, on the shipped binary:
//
//   [G418:unpack] 7.3 calls/f x 0.045-0.057 ms            = 0.39 ms/f   (readback -> VRAM publish)
//   [G498:ownpub] 2.8 calls/f x 0.275 ms, 82.5% fast rows = 0.42 ms/f   (owner publish, fast arm)
//   [G419:pack]   TOTmsF                                   = 0.42 ms/f   (VRAM -> fbPixels upload)
//                                                          ----------
//                                                            1.23 ms/f addressable
//
// ⭐ THE MECHANISM — two GS rows at a time make the destination CONTIGUOUS.
//
// addrPSMCT32(block, width, x, y) =
//     (page << 13) + ((blockId >> 5) << 13) + (blockId & 31) * 256 + columnTable32[y&7][x&7] * 4
//     page    = (block >> 5) + (y >> 5) * width + (x >> 6)
//     blockId = (block & 31) + blockTable32[(y >> 3) & 3][(x >> 3) & 7]
//
// Take y EVEN. Then y&7 in {0,2,4,6}, so (y+1)>>3 == y>>3 and (y+1)>>5 == y>>5: rows y and y+1
// share `page` AND `blockId` exactly. Only the column term differs. Writing C := columnTable32[y&7][0]
// (which is 0, 16, 32 or 48), the table satisfies, for k = 0..7:
//
//     columnTable32[y&7    ][k] = C + 4*(k>>1) + (k&1)
//     columnTable32[(y&7)+1][k] = C + 4*(k>>1) + (k&1) + 2
//
// (verified against ps2_gs_psmct32.h for all four even rows). For an 8-ALIGNED x group, x..x+7 also
// share `x>>3` and `x>>6`, so the block base B is constant. The 16 words the two rows occupy are
// therefore exactly {C .. C+15} — **64 CONTIGUOUS BYTES at B + C*4**, laid out as
//
//     word 0..3   = (y,x0) (y,x1) (y+1,x0) (y+1,x1)
//     word 4..7   = (y,x2) (y,x3) (y+1,x2) (y+1,x3)
//     word 8..11  = (y,x4) (y,x5) (y+1,x4) (y+1,x5)
//     word 12..15 = (y,x6) (y,x7) (y+1,x6) (y+1,x7)
//
// i.e. a 64-bit-granular interleave of the two source rows — four SSE2 `punpcklqdq`/`punpckhqdq`
// pairs. Sixteen pixels cost 4 loads, 4 shuffles, 4 stores and ONE address computation, against the
// existing form's 8 table lookups and 8 partial-line 8-byte stores.
//
// EXACTNESS. Nothing is approximated:
//   * The 16 destination words are derived from the SAME table the scalar path indexes; the group
//     base is literally `off(x0, y)` because columnTable32[y&7][0] == C.
//   * The page table is a bijection over the page, so those 16 words belong to NO other (x,y). A
//     64-byte store therefore touches exactly the 16 pixels it is given — which is also what keeps
//     `GSRowPool` lanes over disjoint row ranges exact (no lane can clobber another's rows).
//   * BOUNDS: the group is taken only when `groupBase + 64 <= vramSize`. Every pair the scalar loop
//     would test lies at groupBase + {0,8,16,24,32,40,48,56}, so that one test IMPLIES all eight of
//     the original `off + 8 <= vramSize` tests. When it fails the caller runs its untouched scalar
//     loop for both rows, including the original's partial/zero-fill tail behaviour.
//   * ALIGNMENT: page, block and C*4 are all multiples of 64, so `groupBase` is always 64-byte
//     aligned — one whole cache line per store group. Unaligned intrinsics are used anyway, because
//     the alignment of `vram` itself is not this file's contract.
//
// ⚠️ SINGLE-FORM PER ARM (G645/G593 law): the rollback selects a DIFFERENT LOOP at the call site,
// never an in-loop ternary. The arm is read ONCE per call (~10/frame), never per group.
//
// Rollback:  DC2_G672_NO_ROWPAIR=1        Paired gate: DC2_G419_AB=rowpair
// Oracle:    DC2_G672_VERIFY=1            Census:      DC2_G672_STAT=1
// ==============================================================================================

#include <emmintrin.h>

// Defined at global scope in g419_ab_instrument.inc (included later in this TU) and declared at
// global scope in ps2_gs_rasterizer.cpp — the G568 linkage lesson: declaring it here, inside the
// anonymous namespace, would name a different, never-defined symbol.

static bool g672RowPairOn()
{
    const int arm = g672RowPairArm();
    if (arm >= 0)
        return arm == 1;
    static const bool on = !envFlagEnabled("DC2_G672_NO_ROWPAIR");
    return on;
}

static bool g672VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G672_VERIFY");
    return on;
}

static bool g672StatOn()
{
    static const bool on = envFlagEnabled("DC2_G672_STAT");
    return on;
}

// Behaviour-pure coverage census. Counted per GROUP, accumulated into thread-local scalars and
// folded once per call, so the kernel loop itself carries no counter.
// The pack converter is invoked once per GSRowPool LANE, so every field is atomic. Nothing here is
// reached unless DC2_G672_STAT is set.
struct G672Stat
{
    std::atomic<uint64_t> calls{0ull}, pairGroups{0ull}, scalarGroups{0ull}, oddRows{0ull};
};
static G672Stat g_g672Stat[3]; // 0 = unpack (G418), 1 = pack (G419), 2 = ownpub (G498)
static std::atomic<uint64_t> g_g672Reports{0ull};

static void g672Note(int site, uint64_t pairGroups, uint64_t scalarGroups, uint64_t oddRows)
{
    if (site < 0 || site > 2)
        return;
    G672Stat &s = g_g672Stat[site];
    s.calls.fetch_add(1ull, std::memory_order_relaxed);
    s.pairGroups.fetch_add(pairGroups, std::memory_order_relaxed);
    s.scalarGroups.fetch_add(scalarGroups, std::memory_order_relaxed);
    s.oddRows.fetch_add(oddRows, std::memory_order_relaxed);
    const uint64_t reports = g_g672Reports.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((reports % 300ull) != 0ull)
        return;
    static const char *const kName[3] = {"unpack", "pack", "ownpub"};
    std::fprintf(stderr, "[G672:rowpair] reports=%llu on=%d",
                 static_cast<unsigned long long>(reports), g672RowPairOn() ? 1 : 0);
    for (int i = 0; i < 3; ++i)
    {
        const double calls =
            static_cast<double>(g_g672Stat[i].calls.load(std::memory_order_relaxed));
        if (calls <= 0.0)
            continue;
        const double pair =
            static_cast<double>(g_g672Stat[i].pairGroups.load(std::memory_order_relaxed));
        const double scal =
            static_cast<double>(g_g672Stat[i].scalarGroups.load(std::memory_order_relaxed));
        const double odd =
            static_cast<double>(g_g672Stat[i].oddRows.load(std::memory_order_relaxed));
        std::fprintf(stderr, " %s(n=%.0f pairGrp=%.1f scalarGrp=%.1f cover=%.2f%% oddRows=%.2f)",
                     kName[i], calls, pair / calls, scal / calls,
                     (pair + scal) > 0.0 ? 100.0 * pair / (pair + scal) : 0.0, odd / calls);
    }
    std::fprintf(stderr, "\n");
}

// ---------------------------------------------------------------------------------------------
// The kernels. Both take ONE 8-aligned x group of two rows and are only ever reached after the
// caller has proved `groupBase + 64 <= vramSize`.
// ---------------------------------------------------------------------------------------------

// srcA = row y, srcB = row y+1 (host linear, 8 pixels each). dst = vram + groupBase.
static inline void g672StoreGroup(uint8_t *dst, const uint32_t *srcA, const uint32_t *srcB)
{
    const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srcA));
    const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srcB));
    const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srcA + 4));
    const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srcB + 4));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 0), _mm_unpacklo_epi64(a0, b0));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 16), _mm_unpackhi_epi64(a0, b0));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 32), _mm_unpacklo_epi64(a1, b1));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 48), _mm_unpackhi_epi64(a1, b1));
}

// The inverse: 64 contiguous VRAM bytes back out into two host rows of 8 pixels each.
static inline void g672LoadGroup(const uint8_t *src, uint32_t *dstA, uint32_t *dstB)
{
    const __m128i l0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 0));
    const __m128i l1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 16));
    const __m128i l2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 32));
    const __m128i l3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 48));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dstA + 0), _mm_unpacklo_epi64(l0, l1));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dstB + 0), _mm_unpackhi_epi64(l0, l1));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dstA + 4), _mm_unpacklo_epi64(l2, l3));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dstB + 4), _mm_unpackhi_epi64(l2, l3));
}

// DC2_G672_VERIFY — re-derive each of the 16 words through the UNTOUCHED `addrPSMCT32` and compare
// the bytes actually in VRAM against the source pixels. Diagnostic only; it repeats the address
// work deliberately and must never be armed in a timing run.
static void g672VerifyGroup(const uint8_t *vram, uint32_t vramSize, const uint32_t *srcA,
                            const uint32_t *srcB, uint32_t block, uint32_t bw, int x, int y)
{
    uint64_t bad = 0ull;
    for (int k = 0; k < 8; ++k)
    {
        for (int r = 0; r < 2; ++r)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(block, bw,
                                                        static_cast<uint32_t>(x + k),
                                                        static_cast<uint32_t>(y + r));
            if (off + 4u > vramSize)
                continue;
            uint32_t got = 0u;
            std::memcpy(&got, vram + off, 4);
            if (got != (r == 0 ? srcA[k] : srcB[k]))
                ++bad;
        }
    }
    static std::atomic<uint64_t> s_groups{0ull}, s_bad{0ull};
    const uint64_t n = s_groups.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    const uint64_t tot = s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0ull || n <= 4ull || (n % 500000ull) == 0ull)
        std::fprintf(stderr, "[G672:verify] groups=%llu words=%llu bad=%llu last=%llu\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(n * 16ull),
                     static_cast<unsigned long long>(tot),
                     static_cast<unsigned long long>(bad));
}
