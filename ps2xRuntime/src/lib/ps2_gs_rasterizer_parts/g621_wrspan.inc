// ===== G621: the PRODUCER-scoped colour readback window =========================================
//
// WHAT WAS WRONG. A GPU batch that reads back publishes GS rows `rowLo..rowHi`, and that pair is the
// union of the entries' SCISSORS. The scissor is a clip, not a claim: it says where a draw is
// ALLOWED to write, not where it does. On `dungeon1` the gap is most of the transfer.
//
// `[G415:stage]`, shipped G620 binary, `dungeon1`:
//
//   fbp=0x13b/read    n=1205  up=0.097 rb=0.569  rows=416.0 win=416.0 upRows=511.1 draws=1.0
//   fbp=0x13b/read+z  n=2406  up=0.042 rb=0.505  rows=416.0 win=416.0 upRows=192.3 draws=2.0
//
// `rb` is ~44% of the whole GL backend — more than three times the draw loop — and it is spent by
// batches carrying ONE or TWO draws. `[G621:span]` then asked the producer's question, keyed on the
// work-done denominator (every batch that reaches the readback):
//
//   fbp=0x13b   (n=2523, 1 draw)  rdRows=416.0  wrSpan=413.4  keep=99.4%   hist: 99% at full span
//   fbp=0x13b+z (n=7677, 2 draws) rdRows=416.0  wrSpan= 89.7  keep=21.6%   hist: 93% in 1/8..2/8
//
// The single-draw population is a genuine full-screen post-process and this lever leaves it exactly
// alone. The depth-carrying population is **75% of all readback batches and reads 416 rows to write
// ninety**.
//
// THE FIX. Intersect the published window with the union of `max(scY0, e.minY) .. min(scY1, e.maxY)`
// over the batch's non-skipped entries. That pair is NOT new: `g282RowLo/g282RowHi` has carried
// exactly it — and has BEEN the published window — for the g282/g290/g570/g586/g587/g604 alias
// batches since G282. G621 only widens its admission from those special cases to every batch.
//
// EXACTNESS, in three steps:
//   1. `e.minY/e.maxY` come from `crawMinY/crawMaxY`, which G144 capture computes "EXACTLY as
//      drawTriangle/drawSprite (same ofy + scissor clamp) so the band prefilter is tight and NEVER
//      NARROWER than the real coverage". So no row a draw touches can fall outside the union.
//   2. A row outside the union is written by no draw in the batch, so after the draws the FBO holds
//      what it held at batch entry.
//   3. At batch entry the FBO equals guest VRAM over the window: either `uploadFb` copied VRAM into
//      it, or the front end's generation snapshot proved they were already equal. Publishing such a
//      row therefore writes the bytes already there — the identity.
//   Anything the bound cannot describe clears `g621WrUsable` and falls back to the scissor union.
//
// SCOPE. This moves exactly one variable: which rows travel GPU->CPU and get packed into guest VRAM.
// Every other consumer of `rowLo/rowHi` — the FBO snapshot, the coalesced display pend, the page
// generation bumps, the depth window — keeps the wide scissor form, where being wide is the
// conservative direction.
//
// Rollback DC2_G621_NO_WRSPAN=1 · oracle DC2_G621_WRSPAN_VERIFY=1 · counters DC2_G621_WRSPAN_STAT=1.
// ==============================================================================================

static bool g621WrSpanOn()
{
    static const bool on = !envFlagEnabled("DC2_G621_NO_WRSPAN") &&
                           !envFlagDisabled("DC2_G621_WRSPAN");
    return on;
}

static bool g621WrSpanVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G621_WRSPAN_VERIFY");
    return on;
}

static bool g621WrSpanStatOn()
{
    static const bool on = envFlagEnabled("DC2_G621_WRSPAN_STAT");
    return on;
}

static std::atomic<uint64_t> g_g621Batches{0}, g_g621Narrowed{0}, g_g621LegacyRows{0},
    g_g621PubRows{0}, g_g621Unusable{0};
static std::atomic<uint64_t> g_g621VerifyChecks{0}, g_g621VerifyBad{0};

static void g621NoteBatch(uint32_t fbp, bool skipReadback, int rowLo, int rowHi,
                          int pubLo, int pubHi, bool narrowed)
{
    if (skipReadback || rowHi < rowLo)
        return;
    const uint64_t n = g_g621Batches.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g621LegacyRows.fetch_add(static_cast<uint64_t>(rowHi - rowLo + 1),
                               std::memory_order_relaxed);
    g_g621PubRows.fetch_add(static_cast<uint64_t>(pubHi >= pubLo ? pubHi - pubLo + 1 : 0),
                            std::memory_order_relaxed);
    if (narrowed)
        g_g621Narrowed.fetch_add(1u, std::memory_order_relaxed);
    if ((n % 2000u) != 0u)
        return;
    const uint64_t leg = g_g621LegacyRows.load(std::memory_order_relaxed);
    const uint64_t pub = g_g621PubRows.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G621:wrspan] on=%d batches=%llu narrowed=%llu unusable=%llu legacyRows=%llu "
                 "pubRows=%llu keep=%.1f%% lastFbp=0x%x win=%d..%d pub=%d..%d "
                 "verify(checks=%llu bad=%llu)\n",
                 g621WrSpanOn() ? 1 : 0, static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_g621Narrowed.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g621Unusable.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(leg), static_cast<unsigned long long>(pub),
                 leg ? 100.0 * static_cast<double>(pub) / static_cast<double>(leg) : 0.0,
                 fbp, rowLo, rowHi, pubLo, pubHi,
                 static_cast<unsigned long long>(g_g621VerifyChecks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g621VerifyBad.load(std::memory_order_relaxed)));
}

// THE ORACLE (DC2_G621_WRSPAN_VERIFY=1). The claimed property is "publishing the excluded rows is
// the identity on guest VRAM", and this asserts it directly rather than by argument.
//
// On the verify arm the backend was handed the LEGACY window, so `px` holds every row of
// `rowLo..rowHi`. The narrowed pack has already run. Checksum the target's VRAM bytes over the whole
// window, run the LEGACY pack on top, checksum again: if the two agree, the rows the narrowed pack
// skipped carried bytes VRAM already had. The legacy pack runs LAST, so the verify arm always ships
// the pre-G621 bytes — an oracle can report a defect, never cause one.
static void g621VerifyExcluded(uint8_t *vram, uint32_t vramSize,
                               const std::vector<uint32_t> &px, uint32_t fbBlock, uint32_t fbw,
                               int fbW, int fbH, int rowLo, int rowHi, int pubLo, int pubHi,
                               uint32_t fbp, bool legacyArm)
{
    if (vram == nullptr || rowHi < rowLo || fbW <= 0)
        return;
    const G144BlockRange win = g144RangeForRect(
        fbBlock, GS_PSM_CT32, fbw, 0u, static_cast<uint32_t>(rowLo),
        static_cast<uint32_t>(fbW), static_cast<uint32_t>(rowHi - rowLo + 1));
    if (!win.valid)
        return;
    const uint64_t lo = static_cast<uint64_t>(win.first) * 256ull;
    const uint64_t hiEnd = (static_cast<uint64_t>(win.last) + 1ull) * 256ull;
    if (hiEnd > static_cast<uint64_t>(vramSize) || hiEnd <= lo)
        return;
    // FNV-1a over the window's bytes — order-sensitive, so a swapped pair of rows is caught too.
    const auto sum = [&]() -> uint64_t {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t i = lo; i < hiEnd; ++i)
        {
            h ^= static_cast<uint64_t>(vram[i]);
            h *= 1099511628211ull;
        }
        return h;
    };
    const uint64_t before = sum();
    g418UnpackColorRows(vram, vramSize, px, fbBlock, fbw, fbW, fbH, rowLo, rowHi, legacyArm);
    const uint64_t after = sum();
    const uint64_t n = g_g621VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (before != after)
    {
        const uint64_t bad = g_g621VerifyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad <= 8u)
            std::fprintf(stderr,
                         "[G621:wrspan-verify] MISMATCH fbp=0x%x win=%d..%d pub=%d..%d fbw=%u "
                         "fbW=%d bad=%llu of %llu — the excluded rows were NOT the identity\n",
                         fbp, rowLo, rowHi, pubLo, pubHi, fbw, fbW,
                         static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(n));
    }
    if ((n % 2000u) == 0u)
        std::fprintf(stderr, "[G621:wrspan-verify] checks=%llu bad=%llu\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(
                         g_g621VerifyBad.load(std::memory_order_relaxed)));
}
