// ===== G477: swizzle-row hoisted IMAGE run writers (exact by construction) =====
//
// PREMISE (priced BEFORE the lever was written — the G476 law). `DC2_G313_CENSUS=1` on lean MAP-0,
// uncapped, on the current binary:
//
//   [G313:psm] psm=0x00 calls=1086044 bytes=11197287552 ms=3716 MiBps=2873   (PSMCT32)
//   [G313:psm] psm=0x14 calls=11680   bytes=574095360   ms=2886 MiBps=189    (PSMT4)
//   [G313:seg] calls=1097728 range=37946 gen=23.8 bump=89.7 pre=30.3 writer=6604 commit=32.5 ms
//
// Over ~3,050 frames that is **writer = 2.17 ms/f of pure GS-worker CPU** (CT32 1.22, T4 0.95) —
// the largest exactly-attributable named CPU block on the pole thread, matching the
// `[G446:gsprof]` rows `GSMem::WriteRunCT32` 4.39% + `GSMem::WriteRunP4` 3.92%. Everything else in
// `processImageData` is noise (gen/bump/pre/commit together are 0.06 ms/f); `range` is
// `g144FlushPendingUploadRange`, i.e. the deferred-draw replay plus its GL round trips, which is
// the already-closed blocked half.
//
// ROOT. `G424RunWrite` calls `PixelStorageTraits<psm>::Write(table, data, bp, bw, x + i, y, v)`
// once per pixel. Every one of those re-derives `Address()` from scratch: `PageId` (a divide-by-
// constant tree plus a `bw` multiply), `block % 32`, `y % pageH`, `x % pageW`, and then a THREE-
// dimensional index into a 128 KB (CT32) / 1 MB (P4) lookup table. But for a whole horizontal run
// at fixed `y` and fixed `bp`, `table[bp % 32][y % pageH]` is ONE fixed row, and the page index is
// constant across each 64-pixel (CT32) / 128-pixel (P4) segment. MSVC cannot hoist any of it: the
// run loop and the traits code are both `constexpr`-inlined here, but the compiler keeps the whole
// address expression live because `x + i` feeds the 3-D subscript.
//
// MECHANISM.
//  1. Resolve the swizzle ROW pointer and the base page ONCE per run.
//  2. Walk the run in page-aligned segments; inside a segment the only per-pixel work is one `u16`
//     table load and an add.
//  3. CT32 only: `ColumnTable32` places consecutive x in ADJACENT PAIRS (`0,1  4,5  8,9  12,13`),
//     so two consecutive pixels are 8 contiguous bytes in VRAM. The pair is detected from the table
//     itself at runtime (`row[k+1] == row[k]+1`) — no layout is assumed — and written as one
//     8-byte store, halving the address work and the store count.
//
// EXACTNESS. The byte address is produced by the identical `u32` expression the traits path uses
// (`bits = addr * UnpackedBitWidth; byte = (bits / 8) & (MEMORY_SIZE - sizeof(PackedT))`), so it
// agrees bit-for-bit including on overflow/wrap. The pair store is only taken when the second
// pixel's INDEPENDENTLY computed masked byte address equals the first plus four, which is checked
// once per segment against the row's maximum, so a run that wraps the VRAM mask falls back to the
// single-pixel form. P4's read-modify-write nibble rule is reproduced verbatim.
//
// Oracle: `DC2_G477_VERIFY=1` recomputes `PixelStorageTraits<psm>::Address()` for EVERY pixel the
// fast loop writes and compares the resulting masked byte address (and, for P4, the nibble shift).
// Rollback: `DC2_G477_NO_FAST_RUN=1` restores the G424 per-pixel run loop.

namespace
{
    bool g477FastRunOn()
    {
        static const bool v = (std::getenv("DC2_G477_NO_FAST_RUN") == nullptr);
        return v;
    }

    bool g477VerifyOn()
    {
        static const bool v = (std::getenv("DC2_G477_VERIFY") != nullptr);
        return v;
    }

    // Paired within-process gate. -1 = not selected, so the writer takes its own default/rollback.
    inline bool g477UseFast()
    {
        const int arm = g477RunWriteArm();
        return (arm >= 0) ? (arm != 0) : g477FastRunOn();
    }

    // ---- DC2_G477_CENSUS: price the site's CALL RATE before believing any profile row (G476) ----
    bool g477CensusOn()
    {
        static const bool v = (std::getenv("DC2_G477_CENSUS") != nullptr);
        return v;
    }

    bool g652P4BulkOn()
    {
        // Prototype only. Live dragon census found every P4 segment was already a complete
        // 128-pixel G477 row (3.56M bulk segments, zero scalar), leaving zero partial groups for
        // this arm. Keep it reproducible without adding a default-path branch/code-layout risk.
        static const bool v = std::getenv("DC2_G652_P4BULK") != nullptr &&
                              std::getenv("DC2_G652_NO_P4BULK") == nullptr;
        return v;
    }

    struct G477RunStat
    {
        unsigned long long calls = 0ull;
        unsigned long long pixels = 0ull;
        unsigned long long segments = 0ull;
    };
    G477RunStat g_g477Stat[2]; // 0 = CT32/Z32, 1 = P4  (GS worker only; single writer thread)
    unsigned long long g_g477P4Quad = 0ull;   // P4 segments taking the 32-bit quad-gather path
    unsigned long long g_g477P4Scalar = 0ull; // P4 segments falling back to the byte RMW loop
    unsigned long long g_g652P4Groups = 0ull; // 32-pixel groups handled by the G652 bulk arm
    unsigned long long g_g652P4Pixels = 0ull;

    void g477NoteRun(unsigned slot, u32 count)
    {
        G477RunStat &s = g_g477Stat[slot];
        ++s.calls;
        s.pixels += count;
        if (((g_g477Stat[0].calls + g_g477Stat[1].calls) % 500000ull) == 0ull)
            std::fprintf(stderr,
                         "[G477:census] ct32(calls=%llu px=%llu avg=%.1f) p4(calls=%llu px=%llu avg=%.1f)"
                         " p4seg(quad=%llu scalar=%llu) g652(groups=%llu px=%llu)\n",
                         g_g477Stat[0].calls, g_g477Stat[0].pixels,
                         g_g477Stat[0].calls ? (double)g_g477Stat[0].pixels / (double)g_g477Stat[0].calls : 0.0,
                         g_g477Stat[1].calls, g_g477Stat[1].pixels,
                         g_g477Stat[1].calls ? (double)g_g477Stat[1].pixels / (double)g_g477Stat[1].calls : 0.0,
                         g_g477P4Quad, g_g477P4Scalar, g_g652P4Groups, g_g652P4Pixels);
    }

    std::atomic<unsigned long long> g_g477Ok{0ull};
    std::atomic<unsigned long long> g_g477Bad{0ull};

    void g477Note(const char *what, u32 bp, u32 bw, u32 x, u32 y, u32 got, u32 ref)
    {
        if (got == ref)
        {
            const unsigned long long ok =
                g_g477Ok.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
            if ((ok % 20000000ull) == 0ull)
                std::fprintf(stderr, "[G477:verify] ok=%llu bad=%llu\n",
                             ok, g_g477Bad.load(std::memory_order_relaxed));
            return;
        }
        const unsigned long long bad =
            g_g477Bad.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if (bad <= 32ull)
            std::fprintf(stderr,
                         "[G477:verify-BAD] %s bp=0x%x bw=%u x=%u y=%u got=0x%x ref=0x%x\n",
                         what, bp, bw, x, y, got, ref);
    }

    // ---- PSMCT32 / PSMZ32 group eligibility -----------------------------------------------------
    //
    // `ColumnTable32` row 0 is `{0,1,4,5,8,9,12,13}`: an 8-pixel group at an 8-aligned x maps to
    // word offsets `w, w+1, w+4, w+5, w+8, w+9, w+12, w+13` — four 8-byte PAIRS at a fixed 16-byte
    // stride from ONE base. So a whole aligned group costs one table load, one address, and four
    // 8-byte stores. Verified once per (block, page row); a row that fails falls back to the
    // data-driven pair loop, which itself falls back to the exact single-pixel form.
    template <typename TableT>
    struct G477C32GroupTable
    {
        u8 ok[32][32]{};

        explicit G477C32GroupTable(const TableT &table)
        {
            for (u32 b = 0u; b < 32u; ++b)
            {
                for (u32 r = 0u; r < 32u; ++r)
                {
                    const auto &row = table[b][r];
                    bool good = true;
                    for (u32 g = 0u; good && g < 64u; g += 8u)
                    {
                        for (u32 k = 0u; k < 4u; ++k)
                        {
                            if (row[g + 2u * k] != row[g] + 4u * k ||
                                row[g + 2u * k + 1u] != row[g] + 4u * k + 1u)
                            {
                                good = false;
                                break;
                            }
                        }
                    }
                    ok[b][r] = good ? 1u : 0u;
                }
            }
        }
    };

    template <PixelStorageMode psm, typename TableT>
    bool g477C32GroupOk(const TableT &table, u32 block, u32 rowIndex)
    {
        static const G477C32GroupTable<TableT> built(table);
        return built.ok[block][rowIndex] != 0u;
    }

    // ---- PSMCT32 / PSMZ32 -----------------------------------------------------------------------
    // `table` is PageTableC32 or PageTableZ32; the two share every geometry constant.
    template <PixelStorageMode psm, bool Verify, typename TableT>
    inline void g477RunWrite32(const TableT &table, u8 *data, u32 bp, u32 bw,
                               u32 x, u32 y, const u8 *src, u32 count)
    {
        using Traits = PixelStorageTraits<psm>;
        constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 4u;
        constexpr u32 kPageW = 64u;
        constexpr u32 kPageH = 32u;
        constexpr u32 kPagePixels = kPageW * kPageH;
        // Highest value any entry of the page table can hold: (31 + 31) * 64 + 63.
        constexpr u32 kRowMax = 4031u;

        const auto &row = table[bp % 32u][y % kPageH];
        const u32 basePage = (bp / 32u) + (y / kPageH) * bw;
        const bool groupOk = g477C32GroupOk<psm>(table, bp % 32u, y % kPageH);

        u32 i = 0u;
        while (i < count)
        {
            const u32 xi = x + i;
            const u32 pageBase = (basePage + (xi / kPageW)) * kPagePixels;
            const u32 idx = xi & (kPageW - 1u);
            u32 lim = kPageW - idx;
            if (lim > count - i)
                lim = count - i;

            // Segment guard: with the largest possible table entry still inside the linear VRAM
            // word range, `(addr * 32) / 8` never overflows and the mask is the identity, so
            // address+1 is byte address+4 and the pair store is exact.
            const bool pairSafe = (pageBase <= (kMask / 4u) - (kRowMax + 1u));

            u32 j = 0u;
            if (pairSafe)
            {
                // Aligned 8-pixel groups: one table load and four 8-byte stores at a 16-byte stride.
                if (groupOk)
                {
                    while (((idx + j) & 7u) != 0u && j + 1u < lim)
                    {
                        const u32 a = row[idx + j];
                        const u32 b = (pageBase + a) * 4u;
                        if (row[idx + j + 1u] == a + 1u)
                        {
                            std::memcpy(data + b, src + (i + j) * 4u, 8u);
                            if constexpr (Verify)
                            {
                                g477Note("ct32", bp, bw, xi + j, y, b,
                                         ((Traits::Address(table, bp, bw, xi + j, y) * 32u) / 8u) & kMask);
                                g477Note("ct32", bp, bw, xi + j + 1u, y, b + 4u,
                                         ((Traits::Address(table, bp, bw, xi + j + 1u, y) * 32u) / 8u) & kMask);
                            }
                            j += 2u;
                            continue;
                        }
                        std::memcpy(data + b, src + (i + j) * 4u, 4u);
                        if constexpr (Verify)
                            g477Note("ct32", bp, bw, xi + j, y, b,
                                     ((Traits::Address(table, bp, bw, xi + j, y) * 32u) / 8u) & kMask);
                        j += 1u;
                    }
                    while (j + 8u <= lim)
                    {
                        const u32 b = (pageBase + row[idx + j]) * 4u;
                        const u8 *s = src + (i + j) * 4u;
                        std::memcpy(data + b, s, 8u);
                        std::memcpy(data + b + 16u, s + 8u, 8u);
                        std::memcpy(data + b + 32u, s + 16u, 8u);
                        std::memcpy(data + b + 48u, s + 24u, 8u);
                        if constexpr (Verify)
                        {
                            for (u32 k = 0u; k < 8u; ++k)
                                g477Note("ct32g", bp, bw, xi + j + k, y, b + (k >> 1u) * 16u + (k & 1u) * 4u,
                                         ((Traits::Address(table, bp, bw, xi + j + k, y) * 32u) / 8u) & kMask);
                        }
                        j += 8u;
                    }
                }
                while (j + 1u < lim)
                {
                    const u32 a = row[idx + j];
                    if (row[idx + j + 1u] == a + 1u)
                    {
                        const u32 b = (pageBase + a) * 4u;
                        std::memcpy(data + b, src + (i + j) * 4u, 8u);
                        if constexpr (Verify)
                        {
                            g477Note("ct32", bp, bw, xi + j, y, b,
                                     ((Traits::Address(table, bp, bw, xi + j, y) * 32u) / 8u) & kMask);
                            g477Note("ct32", bp, bw, xi + j + 1u, y, b + 4u,
                                     ((Traits::Address(table, bp, bw, xi + j + 1u, y) * 32u) / 8u) & kMask);
                        }
                        j += 2u;
                        continue;
                    }
                    const u32 b = (pageBase + a) * 4u;
                    std::memcpy(data + b, src + (i + j) * 4u, 4u);
                    if constexpr (Verify)
                        g477Note("ct32", bp, bw, xi + j, y, b,
                                 ((Traits::Address(table, bp, bw, xi + j, y) * 32u) / 8u) & kMask);
                    j += 1u;
                }
            }
            for (; j < lim; ++j)
            {
                const u32 addr = pageBase + row[idx + j];
                const u32 b = ((addr * 32u) / 8u) & kMask;
                std::memcpy(data + b, src + (i + j) * 4u, 4u);
                if constexpr (Verify)
                    g477Note("ct32", bp, bw, xi + j, y, b,
                             ((Traits::Address(table, bp, bw, xi + j, y) * 32u) / 8u) & kMask);
            }
            i += lim;
        }
    }

    // ---- PSMT4 quad-gather eligibility ----------------------------------------------------------
    //
    // `ColumnTable4` row 0 is `{0,8,32,40,64,72,96,104, 2,10,34,42,…}`: inside each 32-column group
    // the four sub-blocks at x, x+8, x+16, x+24 hold nibble indices n, n+2, n+4, n+6 — i.e. FOUR
    // CONSECUTIVE BYTES at the same nibble half. So four pixels of one run can be assembled into a
    // single 32-bit masked read-modify-write instead of four separate byte RMWs.
    //
    // The relation is a property of the immutable page table, so it is verified ONCE per
    // (block, page row) rather than per pixel. A row that does not satisfy it falls back to the
    // scalar loop, which makes the fast path exact regardless of what the tables contain.
    //
    // Reordering is safe: within one run every pixel owns a DISTINCT nibble (the page table is a
    // bijection over the page), and each RMW preserves every nibble it does not own.
    struct G477P4QuadTable
    {
        u8 ok[32][128]{};

        G477P4QuadTable()
        {
            for (u32 b = 0u; b < 32u; ++b)
            {
                for (u32 r = 0u; r < 128u; ++r)
                {
                    const auto &row = PageTableP4[b][r];
                    bool good = true;
                    for (u32 c = 0u; good && c < 128u; ++c)
                    {
                        if ((c & 31u) >= 8u)
                            continue; // only the eight group anchors seed a quad
                        for (u32 q = 1u; q < 4u; ++q)
                        {
                            if (row[c + q * 8u] != row[c] + 2u * q)
                            {
                                good = false;
                                break;
                            }
                        }
                    }
                    ok[b][r] = good ? 1u : 0u;
                }
            }
        }
    };

    bool g477P4QuadOk(u32 block, u32 rowIndex)
    {
        static const G477P4QuadTable table;
        return table.ok[block][rowIndex] != 0u;
    }

    // ---- PSMT4 ---------------------------------------------------------------------------------
    // `nibble0` is the index of the first source nibble; nibble n lives in `src[n / 2]`, low half
    // first — the order the legacy byte loop consumed them (G424).
    template <bool Verify>
    inline void g477RunWriteP4(u8 *data, u32 bp, u32 bw, u32 x, u32 y,
                               const u8 *src, u32 nibble0, u32 count)
    {
        using Traits = PixelStorageTraits<P4>;
        constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE) - 1u;
        constexpr u32 kPageW = 128u;
        constexpr u32 kPageH = 128u;
        constexpr u32 kPagePixels = kPageW * kPageH;

        const auto &row = PageTableP4[bp % 32u][y % kPageH];
        const u32 basePage = (bp / 32u) + (y / kPageH) * bw;
        const bool censusOn = g477CensusOn();

        u32 i = 0u;
        while (i < count)
        {
            const u32 xi = x + i;
            const u32 pageBase = (basePage + (xi / kPageW)) * kPagePixels;
            const u32 idx = xi & (kPageW - 1u);
            u32 lim = kPageW - idx;
            if (lim > count - i)
                lim = count - i;

            const auto writeScalar = [&](u32 j)
            {
                const u32 n = nibble0 + i + j;
                const u32 v = (static_cast<u32>(src[n >> 1u]) >> ((n & 1u) * 4u)) & 0x0Fu;

                const u32 bits = (pageBase + row[idx + j]) * 4u;
                const u32 b = (bits / 8u) & kMask;
                const u32 sh = bits % 8u;

                data[b] = static_cast<u8>((data[b] & ~(0x0Fu << sh)) | (v << sh));

                if constexpr (Verify)
                {
                    const u32 refBits = Traits::Address(PageTableP4, bp, bw, xi + j, y) * 4u;
                    g477Note("p4", bp, bw, xi + j, y, b, (refBits / 8u) & kMask);
                    g477Note("p4sh", bp, bw, xi + j, y, sh, refBits % 8u);
                }
            };

            // G652 P11: the table relation repeats every 32 columns. G477 used it only when the
            // caller supplied an entire 128-pixel page row; apply the same verified four-nibble
            // gather to every complete 32-pixel group inside a partial run.
            const bool tableOk = g477P4QuadOk(bp % 32u, y % kPageH);
            u32 firstGroup = (idx + 31u) & ~31u;
            u32 groupEnd = (idx + lim) & ~31u;
            const bool oldWholeRow = idx == 0u && lim == kPageW;
            const bool bulkGroups = tableOk &&
                (oldWholeRow || (g652P4BulkOn() && firstGroup < groupEnd));
            if (censusOn)
            {
                if (bulkGroups) ++g_g477P4Quad; else ++g_g477P4Scalar;
            }
            if (!bulkGroups)
            {
                for (u32 j = 0u; j < lim; ++j)
                    writeScalar(j);
                i += lim;
                continue;
            }

            if (oldWholeRow)
            {
                firstGroup = 0u;
                groupEnd = kPageW;
            }
            for (u32 j = 0u; idx + j < firstGroup; ++j)
                writeScalar(j);

            for (u32 group = firstGroup; group < groupEnd; group += 32u)
            {
                if (censusOn && g652P4BulkOn() && !oldWholeRow)
                {
                    ++g_g652P4Groups;
                    g_g652P4Pixels += 32u;
                }
                for (u32 lane = 0u; lane < 8u; ++lane)
                {
                    const u32 c = group + lane;
                    const u32 j0 = c - idx;
                    const u32 bits = (pageBase + row[c]) * 4u;
                    const u32 b = (bits / 8u) & kMask;
                    const u32 sh = bits % 8u;
                    if ((((pageBase + row[c] + 6u) * 4u) / 8u & kMask) != b + 3u)
                    {
                        for (u32 q = 0u; q < 4u; ++q)
                            writeScalar(j0 + q * 8u);
                        continue;
                    }

                    const u32 n0 = nibble0 + i + j0;
                    const u32 half = (n0 & 1u) * 4u;
                    const u8 *sp = src + (n0 >> 1u);
                    const u32 ins = ((static_cast<u32>(sp[0]) >> half) & 0x0Fu) |
                                    (((static_cast<u32>(sp[4]) >> half) & 0x0Fu) << 8u) |
                                    (((static_cast<u32>(sp[8]) >> half) & 0x0Fu) << 16u) |
                                    (((static_cast<u32>(sp[12]) >> half) & 0x0Fu) << 24u);
                    u32 w;
                    std::memcpy(&w, data + b, 4u);
                    w = (w & ~(0x0F0F0F0Fu << sh)) | (ins << sh);
                    std::memcpy(data + b, &w, 4u);

                    if constexpr (Verify)
                    {
                        for (u32 q = 0u; q < 4u; ++q)
                        {
                            const u32 j = j0 + q * 8u;
                            const u32 refBits = Traits::Address(PageTableP4, bp, bw, xi + j, y) * 4u;
                            g477Note("p4q", bp, bw, xi + j, y, b + q, (refBits / 8u) & kMask);
                            g477Note("p4qsh", bp, bw, xi + j, y, sh, refBits % 8u);
                            const u32 n = nibble0 + i + j;
                            g477Note("p4qsrc", bp, bw, xi + j, y,
                                     static_cast<u32>(sp[q * 4u]) >> half & 0x0Fu,
                                     (static_cast<u32>(src[n >> 1u]) >> ((n & 1u) * 4u)) & 0x0Fu);
                        }
                    }
                }
            }
            for (u32 j = groupEnd - idx; j < lim; ++j)
                writeScalar(j);
            i += lim;
        }
    }
}
