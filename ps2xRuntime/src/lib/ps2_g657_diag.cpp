// ===== G657 P3/P4/P7: storage + reporter for the fallback-wall and GPU-coverage census =========
//
// A COLD TU BY CONSTRUCTION (rule 12b), like ps2_g654_layerdiag.cpp and ps2_g656_diag.cpp. The
// accumulators, the name tables and the printf live here; the hot TU sees two out-of-line calls,
// and in the default build not even those.
//
// Compiled ONLY when PS2X_G657_DIAG is on - the file is not in the default source list at all.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>

namespace
{

// ⛔⛔ THESE TABLES ARE COPIES AND THE COPY IS THE HAZARD. The first version of this file carried a
// GUESSED kRejName ordering, and it silently relabelled the phase's biggest finding: index 5 is
// `classify`, and it was printed as `rttcolor`. The census was numerically correct and completely
// unreadable. Both tables below are transcribed from, and must be kept identical to:
//
//   kG266FrName   - rasterizer_rtt_census_and_waves.inc (beside `enum G266FlushRej`)
//   kG262SiteName - rasterizer_rtt_census_and_waves.inc (beside `kG262RejSiteCount`)
//
// kRejIndexClassify below is the ONE index whose value the report branches on, so it is named
// rather than written as a literal.
const char *const kRejName[16] = {
    "none", "off", "cpuonlyrtt", "fbw", "mixedtgt", "classify", "mixedz", "rttdepth",
    "colorzalias", "depthsetup", "texaliasz", "submit", "zreadback", "r13", "r14", "r15"};
enum { kRejIndexClassify = 5 };

// Mirrors kG262SiteName; only meaningful when rej == classify. Indices 8..15 cannot occur
// (kG262RejSiteCount is 8) and are named so a drift shows up as a name rather than as a crash.
const char *const kSiteName[16] = {
    "frame", "atest", "scissor", "blend", "tex", "zpsm", "depth", "legacyz",
    "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15"};

// Mirrors kG528ClsName in g528_flush_publish.inc.
const char *const kClsName[4] = {"disp", "legacy", "discov", "other"};

struct Row
{
    unsigned long long ns;
    unsigned long long entries;
    unsigned long long events;
};

std::mutex g_lock;
std::map<unsigned long long, Row> g_fallback;   // key -> replayed on CPU
std::map<unsigned long long, Row> g_gpuOk;      // key -> admitted to the GPU path

std::atomic<unsigned long long> g_fallbackN{0};
std::atomic<unsigned long long> g_gpuOkN{0};

// key layout: rej<<24 | site<<20 | cls<<16 | fbp (9 bits, zero-extended)
unsigned long long makeKey(unsigned rej, unsigned site, unsigned cls, unsigned fbp)
{
    return (static_cast<unsigned long long>(rej & 0xFu) << 24) |
           (static_cast<unsigned long long>(site & 0xFu) << 20) |
           (static_cast<unsigned long long>(cls & 0x3u) << 16) |
           static_cast<unsigned long long>(fbp & 0x1FFu);
}

bool envOn(const char *name)
{
    const char *e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

void dumpTable(const char *tag, std::map<unsigned long long, Row> &m, double totalMs,
               bool withRej)
{
    std::vector<std::pair<unsigned long long, Row>> rows(m.begin(), m.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<unsigned long long, Row> &a,
                 const std::pair<unsigned long long, Row> &b) { return a.second.ns > b.second.ns; });
    for (const auto &kv : rows)
    {
        const unsigned rej = static_cast<unsigned>((kv.first >> 24) & 0xFu);
        const unsigned site = static_cast<unsigned>((kv.first >> 20) & 0xFu);
        const unsigned cls = static_cast<unsigned>((kv.first >> 16) & 0x3u);
        const unsigned fbp = static_cast<unsigned>(kv.first & 0x1FFu);
        const double ms = kv.second.ns / 1e6;
        const double perEntry = kv.second.entries
                                    ? (double)kv.second.ns / (double)kv.second.entries
                                    : 0.0;
        if (withRej)
            std::fprintf(stderr,
                         "[%s] ms=%.1f share=%.2f%% rej=%s site=%s cls=%s fbp=%03x "
                         "events=%llu entries=%llu nsPerEntry=%.0f entriesPerEvent=%.1f\n",
                         tag, ms, totalMs > 0.0 ? 100.0 * ms / totalMs : 0.0,
                         kRejName[rej],
                         (rej == static_cast<unsigned>(kRejIndexClassify)) ? kSiteName[site] : "-",
                         kClsName[cls], fbp,
                         (unsigned long long)kv.second.events,
                         (unsigned long long)kv.second.entries, perEntry,
                         kv.second.events ? (double)kv.second.entries / (double)kv.second.events
                                          : 0.0);
        else
            std::fprintf(stderr,
                         "[%s] ms=%.1f share=%.2f%% cls=%s fbp=%03x events=%llu entries=%llu "
                         "nsPerEntry=%.0f\n",
                         tag, ms, totalMs > 0.0 ? 100.0 * ms / totalMs : 0.0,
                         kClsName[cls], fbp, (unsigned long long)kv.second.events,
                         (unsigned long long)kv.second.entries, perEntry);
    }
}

} // namespace

// The hot TU reaches these through ps2_g657_diag_api.inc; this TU does not include it (it must not
// pull in <chrono> or the rasterizer's include web), so declare the reporter before its first use.
void g657ReportWall();

bool g657WallOn()
{
    static const bool s_on = envOn("DC2_G657_REJWALL");
    return s_on;
}

void g657NoteFallbackWall(unsigned rej, unsigned site, unsigned cls, unsigned fbp,
                          unsigned long long entries, unsigned long long ns)
{
    if (!g657WallOn())
        return;
    const unsigned long long key = makeKey(rej, site, cls, fbp);
    {
        std::lock_guard<std::mutex> g(g_lock);
        Row &r = g_fallback[key];
        r.ns += ns;
        r.entries += entries;
        ++r.events;
    }
    const unsigned long long n = g_fallbackN.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((n % 2048ull) == 0ull)
        g657ReportWall();
}

void g657NoteGpuOk(unsigned cls, unsigned fbp, unsigned long long entries, unsigned long long ns)
{
    if (!g657WallOn())
        return;
    const unsigned long long key = makeKey(0u, 0u, cls, fbp);
    {
        std::lock_guard<std::mutex> g(g_lock);
        Row &r = g_gpuOk[key];
        r.ns += ns;
        r.entries += entries;
        ++r.events;
    }
    g_gpuOkN.fetch_add(1ull, std::memory_order_relaxed);
}

void g657ReportWall()
{
    if (!g657WallOn())
        return;
    std::lock_guard<std::mutex> g(g_lock);
    double cpuMs = 0.0, gpuMs = 0.0;
    unsigned long long cpuEntries = 0ull, gpuEntries = 0ull;
    for (const auto &kv : g_fallback) { cpuMs += kv.second.ns / 1e6; cpuEntries += kv.second.entries; }
    for (const auto &kv : g_gpuOk)    { gpuMs += kv.second.ns / 1e6; gpuEntries += kv.second.entries; }

    // ⭐ THE COVERAGE METRIC (P4). Three denominators, because they answer different questions and
    // a single "% GPU" number hides the one that matters. A route can be 95% GPU by BATCH and
    // still spend 90% of its raster wall on the CPU remainder.
    const double totMs = cpuMs + gpuMs;
    std::fprintf(stderr,
                 "[G657:cover] gpuBatches=%llu cpuBatches=%llu batchPct=%.2f%% | "
                 "gpuEntries=%llu cpuEntries=%llu entryPct=%.2f%% | "
                 "gpuMs=%.1f cpuMs=%.1f wallPct=%.2f%%\n",
                 (unsigned long long)g_gpuOkN.load(std::memory_order_relaxed),
                 (unsigned long long)g_fallbackN.load(std::memory_order_relaxed),
                 (g_gpuOkN.load() + g_fallbackN.load())
                     ? 100.0 * (double)g_gpuOkN.load() /
                           (double)(g_gpuOkN.load() + g_fallbackN.load())
                     : 0.0,
                 (unsigned long long)gpuEntries, (unsigned long long)cpuEntries,
                 (gpuEntries + cpuEntries)
                     ? 100.0 * (double)gpuEntries / (double)(gpuEntries + cpuEntries) : 0.0,
                 gpuMs, cpuMs, totMs > 0.0 ? 100.0 * gpuMs / totMs : 0.0);

    // ⭐ THE RANKING P3 ACTUALLY ASKED FOR. Sorted by WALL, with the entry count printed beside it
    // so the two rankings can be compared in one place instead of across two runs.
    std::fprintf(stderr, "[G657:rejwall] totalCpuMs=%.1f rows=%zu\n", cpuMs, g_fallback.size());
    dumpTable("G657:rejwall", g_fallback, cpuMs, true);
    std::fprintf(stderr, "[G657:gpuwall] totalGpuMs=%.1f rows=%zu\n", gpuMs, g_gpuOk.size());
    dumpTable("G657:gpuwall", g_gpuOk, gpuMs, false);
}
