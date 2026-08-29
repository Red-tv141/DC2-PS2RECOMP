// ===== G662: storage + reporters for the phase's frozen instrument set =========================
//
// A COLD TU BY CONSTRUCTION (rule 12b), exactly like ps2_g654_layerdiag.cpp / ps2_g656_diag.cpp /
// ps2_g660_diag.cpp. The accumulators, the name tables and every printf live here; the hot TU sees
// only out-of-line calls, and in the default build not even those (ps2_g662_diag_api.inc compiles
// to empty inlines).
//
// Compiled ONLY when PS2X_G662_DIAG is on — the file is not in the default source list at all.
//
// The rationale for each counter is in ps2_g662_diag_api.inc; this file holds only the mechanics.

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace
{

// ---------------------------------------------------------------------------------------------
// [G662:zauth] — P10. Per-FBP histogram of CPU-replayed entries that WRITE the private depth
// mirror. No predicate: the reader checks the printed addresses against the known families
// ({0x142, 0x13d, 0x15c, 0x15b}) themselves, because a histogram cannot lie about its own
// classifier the way a `g604DiscovTransientTarget()`-keyed census did in G661.
enum { kZSlots = 24 };
std::atomic<unsigned> g_zFbp[kZSlots];  // stored as fbp+1 so 0 means "empty slot"
std::atomic<unsigned long long> g_zCnt[kZSlots];
std::atomic<unsigned long long> g_zSeen{0};     // THE DENOMINATOR — counted unconditionally
std::atomic<unsigned long long> g_zWrites{0};   // entries writing the mirror
std::atomic<unsigned long long> g_zOverflow{0}; // more than kZSlots distinct FBPs

void zBump(unsigned fbp)
{
    for (int i = 0; i < kZSlots; ++i)
    {
        const unsigned cur = g_zFbp[i].load(std::memory_order_relaxed);
        if (cur == fbp + 1u)
        {
            g_zCnt[i].fetch_add(1ull, std::memory_order_relaxed);
            return;
        }
        if (cur == 0u)
        {
            unsigned expect = 0u;
            if (g_zFbp[i].compare_exchange_strong(expect, fbp + 1u, std::memory_order_relaxed))
            {
                g_zCnt[i].fetch_add(1ull, std::memory_order_relaxed);
                return;
            }
            --i; // slot was taken between the load and the CAS; re-examine it
        }
    }
    g_zOverflow.fetch_add(1ull, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// [G662:matgran] — P6. kG261MatLocal publication granularity ACROSS calls. Within one call there
// is nothing left to batch: g261Materialize already issues ONE g178_backend_read_color over the
// whole dirty row span, ONE g418UnpackColorRows and ONE g178BumpRectImpl. So the only remaining
// batching question is whether consecutive publications could have been ONE publication —
// `sameTarget` is the churn upper bound and `adjacent` is the strictly coalescible subset.
std::atomic<unsigned long long> g_matCalls{0}, g_matRows{0};
std::atomic<unsigned long long> g_matSameTarget{0}; // same target as the immediately previous call
std::atomic<unsigned long long> g_matAdjacent{0};   // ...AND rowLo == prevRowHi + 1
std::atomic<unsigned> g_matLastFbp{0xFFFFFFFFu};
// ⚠️ NOT `-1 << 30`: left-shifting a negative value is undefined behaviour. Any value that cannot
// be `prevRowHi + 1 == rowLo` for a real row index works; -1000000 is unambiguous.
std::atomic<int> g_matLastRowHi{-1000000};
// Row-count histogram: <=8, <=32, <=64, <=128, <=256, >256
std::atomic<unsigned long long> g_matRowHist[6];

// ---------------------------------------------------------------------------------------------
// [G662:lever]
enum { kLeverCount = 1 };
const char *const kLeverName[kLeverCount] = {"blend139"};
std::atomic<unsigned long long> g_lever[kLeverCount];

// ---------------------------------------------------------------------------------------------
// [G662:colspan] — the COLUMN ceiling of the publication readback (Rule 11's unasked axis).
std::atomic<unsigned long long> g_csN{0};
std::atomic<unsigned long long> g_csPxFull{0};  // rows * fbW      — what is transferred TODAY
std::atomic<unsigned long long> g_csPxTight{0}; // rows * colSpan  — what a column bound would send
std::atomic<unsigned long long> g_csFullWidth{0};
// Column-fraction histogram: <=1/8, <=2/8, <=4/8, <=6/8, <1.0, ==1.0
std::atomic<unsigned long long> g_csHist[6];

} // namespace

// ---------------------------------------------------------------------------------------------

void g662NoteCpuReplayEntry(unsigned fbp, unsigned long long zbuf)
{
    // ZBUF is the raw 64-bit register: ZBP in bits 0..8 (page units), PSM in 24..27, ZMSK in 32.
    // Same decode as g603_admission_census.inc, g605_tri_span.inc and G661's own probe.
    // The mirror is ZBP page 0xd0 with PSMZ24 — the tuple every admission predicate in this tree
    // tests (`guestZbpPage == 0x0d0u && guestZpsm == 1u`), and a WRITE means ZMSK is clear.
    const unsigned long long seen = g_zSeen.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    const bool zmsk = ((zbuf >> 32) & 1ull) != 0ull;
    const unsigned zbpPage = static_cast<unsigned>(zbuf & 0x1FFull);
    const unsigned zpsm = static_cast<unsigned>((zbuf >> 24) & 0xFull);
    if (!zmsk && zbpPage == 0x0d0u && zpsm == 1u)
    {
        g_zWrites.fetch_add(1ull, std::memory_order_relaxed);
        zBump(fbp);
    }
    // Keyed to the DENOMINATOR, and printed at n==1 as well as periodically.
    if (seen != 1ull && (seen % 65536ull) != 0ull)
        return;
    const unsigned long long w = g_zWrites.load(std::memory_order_relaxed);
    char buf[768];
    int off = std::snprintf(buf, sizeof(buf),
                            "[G662:zauth] cpuEntriesSeen=%llu mirrorWrites=%llu (%.3f%%) "
                            "overflow=%llu",
                            seen, w, seen ? 100.0 * static_cast<double>(w) /
                                                static_cast<double>(seen)
                                          : 0.0,
                            g_zOverflow.load(std::memory_order_relaxed));
    for (int i = 0; i < kZSlots && off > 0 && off < static_cast<int>(sizeof(buf)); ++i)
    {
        const unsigned f = g_zFbp[i].load(std::memory_order_relaxed);
        if (f == 0u)
            continue;
        const unsigned long long c = g_zCnt[i].load(std::memory_order_relaxed);
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             " fbp=%03x:%llu(%.1f%%)", f - 1u, c,
                             w ? 100.0 * static_cast<double>(c) / static_cast<double>(w) : 0.0);
    }
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
}

void g662NoteMatLocal(unsigned fbp, int rowLo, int rowHi)
{
    const int rows = (rowHi >= rowLo) ? (rowHi - rowLo + 1) : 0;
    const unsigned long long n = g_matCalls.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (rows > 0)
        g_matRows.fetch_add(static_cast<unsigned long long>(rows), std::memory_order_relaxed);
    const int bucket = rows <= 8 ? 0 : rows <= 32 ? 1 : rows <= 64 ? 2
                                   : rows <= 128  ? 3
                                   : rows <= 256  ? 4
                                                  : 5;
    g_matRowHist[bucket].fetch_add(1ull, std::memory_order_relaxed);

    // Flush-owner threads only (the same single-threaded contract g261Materialize already relies
    // on), so a plain load/store pair here is sufficient and cheaper than an RMW.
    if (fbp == g_matLastFbp.load(std::memory_order_relaxed))
    {
        g_matSameTarget.fetch_add(1ull, std::memory_order_relaxed);
        if (rowLo == g_matLastRowHi.load(std::memory_order_relaxed) + 1)
            g_matAdjacent.fetch_add(1ull, std::memory_order_relaxed);
    }
    g_matLastFbp.store(fbp, std::memory_order_relaxed);
    g_matLastRowHi.store(rowHi, std::memory_order_relaxed);

    if (n != 1ull && (n % 8192ull) != 0ull)
        return;
    const unsigned long long r = g_matRows.load(std::memory_order_relaxed);
    const unsigned long long same = g_matSameTarget.load(std::memory_order_relaxed);
    const unsigned long long adj = g_matAdjacent.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G662:matgran] calls=%llu rows=%llu rows/call=%.1f sameTargetAsPrev=%llu (%.2f%%) "
                 "adjacent=%llu (%.2f%%) | rows<=8:%llu <=32:%llu <=64:%llu <=128:%llu "
                 "<=256:%llu >256:%llu\n",
                 n, r, n ? static_cast<double>(r) / static_cast<double>(n) : 0.0,
                 same, 100.0 * static_cast<double>(same) / static_cast<double>(n),
                 adj, 100.0 * static_cast<double>(adj) / static_cast<double>(n),
                 g_matRowHist[0].load(std::memory_order_relaxed),
                 g_matRowHist[1].load(std::memory_order_relaxed),
                 g_matRowHist[2].load(std::memory_order_relaxed),
                 g_matRowHist[3].load(std::memory_order_relaxed),
                 g_matRowHist[4].load(std::memory_order_relaxed),
                 g_matRowHist[5].load(std::memory_order_relaxed));
    std::fflush(stderr);
}

void g662NoteColSpan(unsigned fbp, int fbW, int colLo, int colHi, int rows)
{
    if (fbW <= 0 || rows <= 0 || colHi < colLo)
        return;
    // Clamp to the target: a scissor may legally exceed the framebuffer width, and a span wider
    // than what is transferred would manufacture a prize out of an out-of-bounds coordinate.
    if (colLo < 0)
        colLo = 0;
    if (colHi > fbW - 1)
        colHi = fbW - 1;
    const int span = colHi - colLo + 1;
    const unsigned long long r = static_cast<unsigned long long>(rows);
    const unsigned long long n = g_csN.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    g_csPxFull.fetch_add(r * static_cast<unsigned long long>(fbW), std::memory_order_relaxed);
    g_csPxTight.fetch_add(r * static_cast<unsigned long long>(span), std::memory_order_relaxed);

    const double frac = static_cast<double>(span) / static_cast<double>(fbW);
    int b;
    if (span >= fbW)      { b = 5; g_csFullWidth.fetch_add(1ull, std::memory_order_relaxed); }
    else if (frac <= 0.125) b = 0;
    else if (frac <= 0.25)  b = 1;
    else if (frac <= 0.50)  b = 2;
    else if (frac <= 0.75)  b = 3;
    else                    b = 4;
    g_csHist[b].fetch_add(1ull, std::memory_order_relaxed);

    if (n != 1ull && (n % 4096ull) != 0ull)
        return;
    const unsigned long long full = g_csPxFull.load(std::memory_order_relaxed);
    const unsigned long long tight = g_csPxTight.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G662:colspan] pubs=%llu fullWidthPubs=%llu (%.2f%%) pxFull=%llu pxTight=%llu "
                 "keep=%.2f%% saved=%.2f%% | frac<=1/8:%llu <=2/8:%llu <=4/8:%llu <=6/8:%llu "
                 "<1.0:%llu ==1.0:%llu  (last fbp=%03x fbW=%d cols=%d..%d rows=%d)\n",
                 n, g_csFullWidth.load(std::memory_order_relaxed),
                 100.0 * static_cast<double>(g_csFullWidth.load(std::memory_order_relaxed)) /
                     static_cast<double>(n),
                 full, tight,
                 full ? 100.0 * static_cast<double>(tight) / static_cast<double>(full) : 0.0,
                 full ? 100.0 * static_cast<double>(full - tight) / static_cast<double>(full) : 0.0,
                 g_csHist[0].load(std::memory_order_relaxed),
                 g_csHist[1].load(std::memory_order_relaxed),
                 g_csHist[2].load(std::memory_order_relaxed),
                 g_csHist[3].load(std::memory_order_relaxed),
                 g_csHist[4].load(std::memory_order_relaxed),
                 g_csHist[5].load(std::memory_order_relaxed),
                 fbp, fbW, colLo, colHi, rows);
    std::fflush(stderr);
}

void g662NoteLever(unsigned id)
{
    if (id >= static_cast<unsigned>(kLeverCount))
        return;
    const unsigned long long n = g_lever[id].fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n == 1ull || (n & 0xFFFFull) == 0ull)
    {
        std::fprintf(stderr, "[G662:lever] %s n=%llu\n", kLeverName[id], n);
        std::fflush(stderr);
    }
}
