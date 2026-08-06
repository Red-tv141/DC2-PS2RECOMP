// ===== G498: the OWNER-PUBLISH colour unpack (the two loops G418 never reached) ================
//
// WHY THIS PART EXISTS.
//
// G418 replaced the flush publication's per-pixel `addrPSMCT32` + 4-byte memcpy scatter with an
// exact page-local offset TABLE + paired 8-byte stores + row lanes, and promoted it default-ON
// (109,051,904 pixels, bad=0; -0.31 ms/f). Two OTHER sites publish a colour readback into guest
// VRAM through the byte-identical naive loop, and neither was ever converted:
//
//   * `g345MaterializeSlot`      (rasterizer_gpu_alias_page_view.inc) — 512 x 416 = 212,992 px,
//                                 ONE per frame on lean MAP-0 (the 0x0 / 0x68 `@96/416` readback).
//   * `g289Resolve14aBeforeDirect`(same file)                         — 512 x  96 =  49,152 px,
//                                 ONE per frame (the 0x14a `@96/96` readback).
//
// That is 262,144 `addrPSMCT32` calls + 262,144 four-byte `memcpy`s per frame on the POLE thread —
// the same order as the single site G418 measured at 0.344 ms isolated. `[G316:worker]` puts the GS
// worker's own CPU at 12.8-13.5 ms/f and `[G446:gsprof]` shows no non-fold row above 5.3%, i.e. the
// thread is flat and only a sum of named sites like this one can move it.
//
// THE ONE THING THAT IS DIFFERENT HERE, AND WHY IT IS STILL EXACT.
//
// Both owner-publish loops carry a per-pixel FILTER the flush publication does not:
//
//     block = off >> 8;
//     if (block >= owner.firstOwnedBlock && block <= owner.lastOwnedBlock && off + 4 <= vramSize)
//         memcpy(vram + off, &src[srcRow + x], 4);
//
// so G418's arm cannot simply be dropped in. It can be dropped in for the rows where the filter is
// provably a NO-OP, and that is decidable 64x more cheaply than the filter itself:
//
//   * `addrPSMCT32(block,bw,x,y) >> 8` depends on y only through `y>>3` (it is `(y>>5)` for the page
//     step and `(y>>3)&3` for the blockTable32 row) and on x only through `x>>3` and `x>>6`.
//     Therefore ONE probe at (xg, y0) decides the block for all 8 pixels of the x-group AND for all
//     8 rows of the block-row group. 64 probes classify 8 rows x 512 pixels = 4,096 pixels.
//   * Within a block, `off & 0xFF` is `columnTable32[y&7][x&7] * 4`, whose maximum is 252. So
//     `off + 4 <= vramSize` for EVERY pixel of a block is exactly `(block << 8) + 256 <= vramSize`.
//
// A block-row group whose 64 probes all pass has every pixel passing the original filter, so on
// those rows the filtered loop and G418's unfiltered arm write the same bytes to the same addresses.
// Rows that do not pass run the ORIGINAL loop verbatim. Nothing is approximated and nothing is
// skipped: this is a dispatch decision, not a change of contract.
//
// Levers:
//   DC2_G498_NO_OWNPUB=1   rollback to the untouched per-pixel loop at both sites
//   DC2_G419_AB=ownpub     paired within-process arm (thread-level -> G438 block hold)
//   DC2_G498_VERIFY=1      same-run oracle: re-derive every pixel through the original address path
//                          AND the original filter, then compare the bytes actually in VRAM
//   DC2_G498_STAT=1        per-site call/row/pixel/ns census, plus how many rows took the fast arm
// ==============================================================================================

// g498OwnPubArm() is declared at TRUE global scope in ps2_gs_rasterizer.cpp (beside
// g456UvPredicateArm) and defined in g419_ab_instrument.inc, which this TU includes AFTER this
// part. Declaring it here instead would bind the call to the enclosing anonymous namespace and
// leave nothing to link against — the trap that cost G497 two links.
// SERIAL, deliberately. Measured on this binary, both sites, once each per frame:
//   legacy per-pixel   0.564 + 0.130 = 0.694 ms/f
//   table+paired, 1 lane 0.215 + 0.057 = 0.272 ms/f   <- shipped: 0.422 ms/f of work DELETED
//   table+paired, 8 lanes 0.123 + 0.052 = 0.175 ms/f  <- 0.097 ms/f less WALL on this thread, but it
//                                                        buys that by spending ~0.15 ms of other
//                                                        cores plus G420's fixed ~0.085 ms dispatch,
//                                                        on a 6-physical-core host whose pole is now
//                                                        the VU1 worker (G498 §1). G491 measured
//                                                        directly that freeing a core buys nothing
//                                                        here, and the lanes would add a second
//                                                        concurrent VRAM writer to soak.
// DC2_G498_LANES=<n> re-opens the lane form for a future host without a rebuild.
static int g498Lanes()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G498_LANES");
        if (value == nullptr || value[0] == '\0')
            return 1;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    return n;
}

static bool g498OwnPubActive()
{
    const int arm = g498OwnPubArm();
    if (arm >= 0)
        return arm == 1;
    static const bool on = !envFlagEnabled("DC2_G498_NO_OWNPUB");
    return on;
}

static bool g498VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G498_VERIFY");
    return on;
}

static bool g498StatOn()
{
    static const bool on = envFlagEnabled("DC2_G498_STAT");
    return on;
}

// The untouched original loop. Every arm falls back to this, byte for byte.
static void g498PublishRowsLegacy(uint8_t *vram, uint32_t vramSize,
                                  const std::vector<uint32_t> &src,
                                  uint32_t dstBlock, uint32_t bw, int fbW, int fbH,
                                  int rowLo, int rowHi,
                                  uint32_t firstBlock, uint32_t lastBlock)
{
    for (int y = rowLo; y <= rowHi; ++y)
    {
        const size_t srcRow = static_cast<size_t>(fbH - 1 - y) * static_cast<size_t>(fbW);
        for (int x = 0; x < fbW; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, bw,
                                                        static_cast<uint32_t>(x),
                                                        static_cast<uint32_t>(y));
            const uint32_t block = off >> 8u;
            if (block >= firstBlock && block <= lastBlock && off + 4u <= vramSize)
                std::memcpy(vram + off, &src[srcRow + x], 4u);
        }
    }
}

// True when EVERY pixel of rows [y0, y0+7] passes the original filter, so G418's unfiltered arm is
// byte-equivalent there. `y0` need not be 8-aligned; the classification is a property of `y0 >> 3`
// and the caller only ever asks about rows inside one block-row group.
static bool g498GroupFullyOwned(uint32_t dstBlock, uint32_t bw, int fbW, int probeY,
                                uint32_t vramSize, uint32_t firstBlock, uint32_t lastBlock)
{
    for (int xg = 0; xg < fbW; xg += 8)
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, bw,
                                                    static_cast<uint32_t>(xg),
                                                    static_cast<uint32_t>(probeY));
        const uint32_t block = off >> 8u;
        if (block < firstBlock || block > lastBlock)
            return false;
        // Largest byte offset any pixel of this block can address is (block<<8)+252, and the
        // original loop needs off+4 <= vramSize for it.
        if ((block << 8u) + 256u > vramSize)
            return false;
    }
    return true;
}

struct G498PubStat
{
    uint64_t n = 0, rows = 0, fastRows = 0, px = 0, ns = 0;
};
static G498PubStat s_g498Stat[2]; // 0 = materialize (416 rows), 1 = 14a suffix (96 rows)
static uint64_t s_g498Total = 0ull;

static void g498NotePublish(int site, int rows, int fastRows, int fbW, uint64_t ns)
{
    if (site < 0 || site > 1)
        return;
    G498PubStat &s = s_g498Stat[site];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.fastRows += static_cast<uint64_t>(fastRows);
    s.px += static_cast<uint64_t>(rows) * static_cast<uint64_t>(fbW);
    s.ns += ns;
    if ((++s_g498Total % 300ull) != 0ull)
        return;
    std::fprintf(stderr, "[G498:ownpub] total=%llu fast=%d",
                 static_cast<unsigned long long>(s_g498Total), g498OwnPubActive() ? 1 : 0);
    for (int i = 0; i < 2; ++i)
    {
        const G498PubStat &e = s_g498Stat[i];
        if (e.n == 0ull)
            continue;
        std::fprintf(stderr, " site%d(n=%llu rows=%.1f fastRows=%.1f Mpx=%.3f cpu=%.3fms)",
                     i, static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.rows) / e.n,
                     static_cast<double>(e.fastRows) / e.n,
                     static_cast<double>(e.px) / 1.0e6 / e.n,
                     static_cast<double>(e.ns) / 1.0e6 / e.n);
    }
    std::fprintf(stderr, "\n");
}

// DC2_G498_VERIFY: re-derive every pixel through the ORIGINAL address path AND the ORIGINAL filter,
// then compare the bytes actually in VRAM. Diagnostic-only; it repeats the work deliberately.
static void g498VerifyPublish(const uint8_t *vram, uint32_t vramSize,
                              const std::vector<uint32_t> &src,
                              uint32_t dstBlock, uint32_t bw, int fbW, int fbH,
                              int rowLo, int rowHi,
                              uint32_t firstBlock, uint32_t lastBlock)
{
    uint64_t bad = 0ull, checked = 0ull;
    for (int y = rowLo; y <= rowHi; ++y)
    {
        const size_t srcRow = static_cast<size_t>(fbH - 1 - y) * static_cast<size_t>(fbW);
        for (int x = 0; x < fbW; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(dstBlock, bw,
                                                        static_cast<uint32_t>(x),
                                                        static_cast<uint32_t>(y));
            const uint32_t block = off >> 8u;
            if (block < firstBlock || block > lastBlock || off + 4u > vramSize)
                continue;
            uint32_t got = 0u;
            std::memcpy(&got, vram + off, 4u);
            ++checked;
            if (got != src[srcRow + x])
                ++bad;
        }
    }
    static uint64_t s_n = 0ull, s_px = 0ull, s_bad = 0ull;
    ++s_n;
    s_px += checked;
    s_bad += bad;
    if (bad != 0ull || s_n <= 8ull || (s_n % 128ull) == 0ull)
        std::fprintf(stderr,
                     "[G498:verify] publishes=%llu pixels=%llu bad=%llu last(rows=%d bad=%llu)\n",
                     static_cast<unsigned long long>(s_n),
                     static_cast<unsigned long long>(s_px),
                     static_cast<unsigned long long>(s_bad),
                     rowHi - rowLo + 1, static_cast<unsigned long long>(bad));
}

// The one entry point both owner-publish sites call. `site` selects the census bucket only.
static void g498PublishOwnedRows(int site, uint8_t *vram, uint32_t vramSize,
                                 const std::vector<uint32_t> &src,
                                 uint32_t dstBlock, uint32_t bw, int fbW, int fbH,
                                 int rowLo, int rowHi,
                                 uint32_t firstBlock, uint32_t lastBlock)
{
    if (vram == nullptr || fbW <= 0 || rowHi < rowLo)
        return;
    const bool stat = g498StatOn();
    const auto t0 = stat ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    int fastRows = 0;
    // `bw == 0` would be normalised to 1 inside G418's page table but is passed through verbatim by
    // the legacy loop, so the two would not agree. Keep the original loop for it.
    if (!g498OwnPubActive() || (fbW & 1) != 0 || bw == 0u)
    {
        g498PublishRowsLegacy(vram, vramSize, src, dstBlock, bw, fbW, fbH, rowLo, rowHi,
                              firstBlock, lastBlock);
    }
    else
    {
        // Walk block-row groups, accumulate maximal contiguous runs of fully-owned rows, and hand
        // each run to G418's promoted arm. Anything else runs the original loop verbatim.
        int runLo = -1;
        int y = rowLo;
        while (y <= rowHi)
        {
            const int groupHi = std::min(rowHi, (y | 7));
            const bool owned = g498GroupFullyOwned(dstBlock, bw, fbW, y, vramSize,
                                                   firstBlock, lastBlock);
            if (owned)
            {
                if (runLo < 0)
                    runLo = y;
                fastRows += groupHi - y + 1;
            }
            else
            {
                if (runLo >= 0)
                {
                    g418UnpackColorRows(vram, vramSize, src, dstBlock, bw, fbW, fbH,
                                        runLo, y - 1, false, g498Lanes());
                    runLo = -1;
                }
                g498PublishRowsLegacy(vram, vramSize, src, dstBlock, bw, fbW, fbH,
                                      y, groupHi, firstBlock, lastBlock);
            }
            y = groupHi + 1;
        }
        if (runLo >= 0)
            g418UnpackColorRows(vram, vramSize, src, dstBlock, bw, fbW, fbH,
                                runLo, rowHi, false, g498Lanes());
    }
    if (stat)
        g498NotePublish(site, rowHi - rowLo + 1, fastRows, fbW,
                        static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count()));
    if (g498VerifyOn())
        g498VerifyPublish(vram, vramSize, src, dstBlock, bw, fbW, fbH, rowLo, rowHi,
                          firstBlock, lastBlock);
}
