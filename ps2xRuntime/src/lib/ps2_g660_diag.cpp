// ===== G660 P2/P3/P4: storage + reporter for the `colorzalias` rejection-term census ==========
//
// A COLD TU BY CONSTRUCTION (rule 12b), like ps2_g657_diag.cpp and ps2_g656_diag.cpp. The
// accumulators, the name table and the printf live here; the hot TU sees one out-of-line call,
// and in the default build not even that.
//
// Compiled ONLY when PS2X_G660_DIAG is on — the file is not in the default source list at all.
//
// Rationale and the bit meanings live beside the enum in ps2_g660_diag_api.inc.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>

namespace
{

// ⛔ THIS TABLE IS A COPY AND THE COPY IS THE HAZARD — the same trap ps2_g657_diag.cpp records for
// kRejName, where a guessed ordering silently relabelled a phase's biggest finding. Keep these
// names in the SAME ORDER as `enum G660ExactTerm` in ps2_g660_diag_api.inc.
const char *const kTermName[11] = {
    "gateOff",      // 1<<0  g570Private139On() is default-OFF
    "fbp",          // 1<<1
    "fbw",          // 1<<2
    "psm",          // 1<<3
    "noGuestDepth", // 1<<4
    "depthWrites",  // 1<<5   CORRECTNESS-RELEVANT
    "zbp",          // 1<<6
    "zpsm",         // 1<<7
    "rowWindow",    // 1<<8
    "sharedGpuZ",   // 1<<9
    "zScope",       // 1<<10  CORRECTNESS-RELEVANT
};

// The two terms that would make the reject a genuine correctness requirement rather than a
// conservative admission restriction: the batch actually WRITES depth through the aliased pages,
// or the depth it touches is NOT the G403/G411 private mirror.
constexpr unsigned kCorrectnessMask = (1u << 5) | (1u << 10);

struct Row
{
    unsigned long long batches = 0;
    unsigned long long entries = 0;
    unsigned long long zAliasLive = 0;   // batches whose depth is real aliased VRAM
    unsigned long long depthWriting = 0; // batches with guestDepthWrites
    int rowLo = 1 << 30, rowHi = -(1 << 30);
    int dRowLo = 1 << 30, dRowHi = -(1 << 30);
};

std::mutex g_lock;
// key: termMask<<24 | prim<<20 | tme<<19 | abe<<18 | zpsm<<14 | fbp(9 bits)
std::map<unsigned long long, Row> g_rows;
std::atomic<unsigned long long> g_n{0};

unsigned long long makeKey(unsigned termMask, unsigned prim, unsigned tme, unsigned abe,
                           unsigned zpsm, unsigned fbp)
{
    return (static_cast<unsigned long long>(termMask & 0x7FFu) << 24) |
           (static_cast<unsigned long long>(prim & 0x7u) << 20) |
           (static_cast<unsigned long long>(tme & 0x1u) << 19) |
           (static_cast<unsigned long long>(abe & 0x1u) << 18) |
           (static_cast<unsigned long long>(zpsm & 0xFu) << 14) |
           static_cast<unsigned long long>(fbp & 0x1FFu);
}

bool envOn(const char *name)
{
    const char *e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

} // namespace

void g660ReportRej();

bool g660RejCensusOn()
{
    static const bool s_on = envOn("DC2_G660_REJTERM");
    return s_on;
}

void g660NoteColorZReject(unsigned fbp, unsigned fbw, unsigned termMask, unsigned zbpPage,
                          unsigned zpsm, int rowLo, int rowHi, int dRowLo, int dRowHi,
                          unsigned prim, unsigned tme, unsigned abe, unsigned zAliasLive,
                          unsigned long long entries)
{
    if (!g660RejCensusOn())
        return;
    (void)fbw;
    (void)zbpPage;
    const unsigned long long key = makeKey(termMask, prim, tme, abe, zpsm, fbp);
    {
        std::lock_guard<std::mutex> g(g_lock);
        Row &r = g_rows[key];
        ++r.batches;
        r.entries += entries;
        r.zAliasLive += zAliasLive ? 1ull : 0ull;
        r.depthWriting += (termMask & (1u << 5)) ? 1ull : 0ull;
        if (rowLo < r.rowLo) r.rowLo = rowLo;
        if (rowHi > r.rowHi) r.rowHi = rowHi;
        if (dRowLo < r.dRowLo) r.dRowLo = dRowLo;
        if (dRowHi > r.dRowHi) r.dRowHi = dRowHi;
    }
    // The route is killed at timeout, so there is no teardown hook to report from (same as
    // ps2_g657_diag.cpp). Report periodically instead.
    //
    // ⚠️ THE PERIOD IS PART OF THE MEASUREMENT. A first pass used 512 and `dragon` printed only the
    // n<=4 lines: that route's 0x139 population lives in its DIALOGUE window, and an uninstrumented
    // run reaches the static tail early and finishes under 512 rejects — so the last visible number
    // described 4 batches while `[G282:colorz]` in a slower run of the same route reached n=1680.
    // The rates were identical, but a reader would have quoted a 4-batch sample as the cross-route
    // gate. Keep this small enough that EVERY route's final cumulative state is printed.
    const unsigned long long n = g_n.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n <= 8ull || (n % 32ull) == 0ull)
        g660ReportRej();
}

void g660ReportRej()
{
    if (!g660RejCensusOn())
        return;
    std::lock_guard<std::mutex> g(g_lock);

    unsigned long long totalBatches = 0, totalEntries = 0;
    unsigned long long correctnessBatches = 0, correctnessEntries = 0;
    unsigned long long gateOnlyBatches = 0, gateOnlyEntries = 0;
    for (const auto &kv : g_rows)
    {
        const unsigned mask = static_cast<unsigned>((kv.first >> 24) & 0x7FFu);
        totalBatches += kv.second.batches;
        totalEntries += kv.second.entries;
        if (mask & kCorrectnessMask)
        {
            correctnessBatches += kv.second.batches;
            correctnessEntries += kv.second.entries;
        }
        // The population this phase cares about: the ONLY false conjunct is the default-OFF env
        // gate, i.e. every correctness term of the exact contract already holds.
        if (mask == (1u << 0))
        {
            gateOnlyBatches += kv.second.batches;
            gateOnlyEntries += kv.second.entries;
        }
    }

    // ⭐ THE ONE LINE THE PRIORITY ASKED FOR: separate correctness-required alias restrictions from
    // conservative admission restrictions, weighted by the population each blocks.
    std::fprintf(stderr,
                 "[G660:rejterm] batches=%llu entries=%llu | gateOnly=%llu/%llu (%.2f%% batches, "
                 "%.2f%% entries) | correctnessRequired=%llu/%llu (%.2f%% batches, %.2f%% entries) "
                 "| rows=%zu\n",
                 (unsigned long long)totalBatches, (unsigned long long)totalEntries,
                 (unsigned long long)gateOnlyBatches, (unsigned long long)totalBatches,
                 totalBatches ? 100.0 * (double)gateOnlyBatches / (double)totalBatches : 0.0,
                 totalEntries ? 100.0 * (double)gateOnlyEntries / (double)totalEntries : 0.0,
                 (unsigned long long)correctnessBatches, (unsigned long long)totalBatches,
                 totalBatches ? 100.0 * (double)correctnessBatches / (double)totalBatches : 0.0,
                 totalEntries ? 100.0 * (double)correctnessEntries / (double)totalEntries : 0.0,
                 g_rows.size());

    std::vector<std::pair<unsigned long long, Row>> rows(g_rows.begin(), g_rows.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<unsigned long long, Row> &a,
                 const std::pair<unsigned long long, Row> &b) {
                  return a.second.entries > b.second.entries;
              });
    for (const auto &kv : rows)
    {
        const unsigned mask = static_cast<unsigned>((kv.first >> 24) & 0x7FFu);
        const unsigned prim = static_cast<unsigned>((kv.first >> 20) & 0x7u);
        const unsigned tme = static_cast<unsigned>((kv.first >> 19) & 0x1u);
        const unsigned abe = static_cast<unsigned>((kv.first >> 18) & 0x1u);
        const unsigned zpsm = static_cast<unsigned>((kv.first >> 14) & 0xFu);
        const unsigned fbp = static_cast<unsigned>(kv.first & 0x1FFu);
        char terms[192];
        int off = 0;
        terms[0] = '\0';
        for (int b = 0; b < 11; ++b)
            if (mask & (1u << b))
                off += std::snprintf(terms + off, sizeof(terms) - (size_t)off, "%s%s",
                                     off ? "+" : "", kTermName[b]);
        if (off == 0)
            std::snprintf(terms, sizeof(terms), "NONE(contract holds)");
        std::fprintf(stderr,
                     "[G660:rejrow] entries=%llu batches=%llu fbp=%03x prim=%u tme=%u abe=%u "
                     "zpsm=%u rows=%d..%d zrows=%d..%d zAliasLive=%llu depthWriting=%llu "
                     "failing=%s\n",
                     (unsigned long long)kv.second.entries,
                     (unsigned long long)kv.second.batches, fbp, prim, tme, abe, zpsm,
                     kv.second.rowLo, kv.second.rowHi, kv.second.dRowLo, kv.second.dRowHi,
                     (unsigned long long)kv.second.zAliasLive,
                     (unsigned long long)kv.second.depthWriting, terms);
    }
}
