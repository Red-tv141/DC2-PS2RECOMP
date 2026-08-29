// ===== G666: storage + reporters for the discovered-target term census and the depth-rate oracle =
//
// A COLD TU BY CONSTRUCTION (rule 12b), like ps2_g660_diag.cpp / ps2_g663_diag.cpp. The
// accumulators, the name tables and the printfs live here; the hot TU sees one out-of-line call per
// rejected batch and one per verified depth batch, and in the default build not even that.
//
// Compiled ONLY when PS2X_G666_DIAG is on — the file is not in the default source list at all.
//
// Rationale and the bit meanings live beside the enums in ps2_g666_diag_api.inc.

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
// kRejName and ps2_g660_diag.cpp for kTermName, where a guessed ordering silently relabelled a
// phase's biggest finding. Keep these names in the SAME ORDER as `enum G666DiscovTerm`.
const char *const kTermName[11] = {
    "width",         // 1<<0   PERFORMANCE-motivated (G662 +0.828 ms/f on the FBW=8 no-depth half)
    "psm",           // 1<<1
    "mixedEntry",    // 1<<2
    "rowWindow",     // 1<<3
    "noDepthPolicy", // 1<<4   THE BLANKET POLICY under test
    "depthWrites",   // 1<<5   correctness-relevant
    "zbp",           // 1<<6   FOREIGN depth authority
    "zpsm",          // 1<<7   FOREIGN depth authority
    "zRowWindow",    // 1<<8
    "sharedGpuZ",    // 1<<9
    "zScope",        // 1<<10  FOREIGN depth authority
};

// The batch's depth is NOT the G403/G411 private mirror. This is the ONLY bucket in which the GPU
// genuinely lacks an authority for the surface — everything else is a policy or a shape.
constexpr unsigned kForeignZMask =
    (1u << 6) | (1u << 7) | (1u << 10);

// The perf gate alone.
constexpr unsigned kWidthMask = (1u << 0);
// The blanket NODEPTH policy alone.
constexpr unsigned kPolicyMask = (1u << 4);

struct TermRow
{
    unsigned long long batches = 0;
    unsigned long long entries = 0;
    unsigned long long depthWriting = 0;
    unsigned long long guestDepthed = 0;
    int rowLo = 1 << 30, rowHi = -(1 << 30);
    int dRowLo = 1 << 30, dRowHi = -(1 << 30);
};

std::mutex g_termLock;
// key: termMask<<26 | prim<<22 | tme<<21 | zw<<20 | gz<<19 | fbw<<12 | fbp(9 bits) ... see makeKey
std::map<unsigned long long, TermRow> g_termRows;
std::atomic<unsigned long long> g_termN{0};

unsigned long long makeTermKey(unsigned termMask, unsigned prim, unsigned tme, unsigned zw,
                               unsigned gz, unsigned fbw, unsigned fbp)
{
    return (static_cast<unsigned long long>(termMask & 0x7FFu) << 26) |
           (static_cast<unsigned long long>(prim & 0x7u) << 22) |
           (static_cast<unsigned long long>(tme & 0x1u) << 21) |
           (static_cast<unsigned long long>(zw & 0x1u) << 20) |
           (static_cast<unsigned long long>(gz & 0x1u) << 19) |
           (static_cast<unsigned long long>(fbw & 0x7Fu) << 12) |
           static_cast<unsigned long long>(fbp & 0x1FFu);
}

// ---- depth-rate oracle storage -----------------------------------------------------------------
constexpr int kZHist = 7; // keep in sync with kG666ZHistBuckets in ps2_g666_diag_api.inc
const char *const kZHistName[kZHist] = {
    "1", "2-15", "16-255", "256-4k", "4k-64k", "64k-1M", ">=1M",
};

struct ZRow
{
    unsigned long long batches = 0;
    unsigned long long rect = 0;         // whole depth rect — the INHERITED denominator
    unsigned long long cpuWrote = 0;
    unsigned long long gpuWrote = 0;
    unsigned long long touched = 0;      // the CORRECT denominator
    unsigned long long badTouched = 0;
    unsigned long long badUntouched = 0; // must be 0; nonzero == authority leak, not a value error
    unsigned long long maxDelta = 0;
    unsigned long long hist[kZHist] = {0, 0, 0, 0, 0, 0, 0};
    int bigX = -1, bigY = -1;
    unsigned bigCpu = 0, bigGpu = 0;
};

std::mutex g_zLock;
std::map<unsigned, ZRow> g_zRows; // key: prim<<12 | fbp
std::atomic<unsigned long long> g_zN{0};

bool envOn(const char *name)
{
    const char *e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

void formatTerms(unsigned mask, char *out, size_t cap)
{
    int off = 0;
    out[0] = '\0';
    for (int b = 0; b < 11; ++b)
        if (mask & (1u << b))
            off += std::snprintf(out + off, cap - static_cast<size_t>(off), "%s%s",
                                 off ? "+" : "", kTermName[b]);
    if (off == 0)
        std::snprintf(out, cap, "NONE(contract holds)");
}

} // namespace

void g666ReportTerms();
void g666ReportZRate();

// ================================ [G666:discovterm] =============================================

bool g666TermCensusOn()
{
    static const bool s_on = envOn("DC2_G666_DISCOVTERM");
    return s_on;
}

void g666NoteDiscovReject(unsigned fbp, unsigned fbw, unsigned termMask, unsigned zbpPage,
                          unsigned zpsm, int rowLo, int rowHi, int dRowLo, int dRowHi,
                          unsigned prim, unsigned tme, unsigned guestDepth, unsigned depthWrites,
                          unsigned long long entries)
{
    if (!g666TermCensusOn())
        return;
    (void)zbpPage;
    (void)zpsm;
    const unsigned long long key =
        makeTermKey(termMask, prim, tme, depthWrites, guestDepth, fbw, fbp);
    {
        std::lock_guard<std::mutex> g(g_termLock);
        TermRow &r = g_termRows[key];
        ++r.batches;
        r.entries += entries;
        r.depthWriting += depthWrites ? 1ull : 0ull;
        r.guestDepthed += guestDepth ? 1ull : 0ull;
        if (rowLo < r.rowLo) r.rowLo = rowLo;
        if (rowHi > r.rowHi) r.rowHi = rowHi;
        if (dRowLo < r.dRowLo) r.dRowLo = dRowLo;
        if (dRowHi > r.dRowHi) r.dRowHi = dRowHi;
    }
    // The route is killed at timeout, so there is no teardown hook to report from (same as
    // ps2_g657_diag.cpp / ps2_g660_diag.cpp). Report periodically instead, and print at n == 1 as
    // well as periodically so a route with a tiny population is not silent (G661 §1.6b rule 3 /
    // G657 measurement law 10 — a census that prints nothing is indistinguishable from a zero).
    const unsigned long long n = g_termN.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n <= 8ull || (n % 256ull) == 0ull)
        g666ReportTerms();
}

void g666ReportTerms()
{
    if (!g666TermCensusOn())
        return;
    std::lock_guard<std::mutex> g(g_termLock);

    unsigned long long totB = 0, totE = 0;
    unsigned long long widthOnlyB = 0, widthOnlyE = 0;
    unsigned long long policyOnlyB = 0, policyOnlyE = 0;
    unsigned long long widthPlusPolicyB = 0, widthPlusPolicyE = 0;
    unsigned long long foreignB = 0, foreignE = 0;
    unsigned long long depthWriteB = 0, depthWriteE = 0;
    unsigned long long roPrivB = 0, roPrivE = 0; // guest depth, NOT writing, private mirror clean
    unsigned long long rwPrivB = 0, rwPrivE = 0; // guest depth, WRITING, private mirror clean

    for (const auto &kv : g_termRows)
    {
        const unsigned mask = static_cast<unsigned>((kv.first >> 26) & 0x7FFu);
        const unsigned zw = static_cast<unsigned>((kv.first >> 20) & 0x1u);
        const unsigned gz = static_cast<unsigned>((kv.first >> 19) & 0x1u);
        const unsigned long long b = kv.second.batches, e = kv.second.entries;
        totB += b; totE += e;
        if (mask == kWidthMask)                      { widthOnlyB += b; widthOnlyE += e; }
        if (mask == kPolicyMask)                     { policyOnlyB += b; policyOnlyE += e; }
        if (mask == (kWidthMask | kPolicyMask))      { widthPlusPolicyB += b; widthPlusPolicyE += e; }
        if (mask & kForeignZMask)                    { foreignB += b; foreignE += e; }
        if (mask & (1u << 5))                        { depthWriteB += b; depthWriteE += e; }
        // "private-mirror clean" = no foreign-Z term, no z row-window term, no sharedGpuZ term.
        const bool privClean =
            (mask & (kForeignZMask | (1u << 8) | (1u << 9))) == 0u;
        if (gz && privClean && !zw)                  { roPrivB += b; roPrivE += e; }
        if (gz && privClean && zw)                   { rwPrivB += b; rwPrivE += e; }
    }

    const double pB = totB ? 100.0 / static_cast<double>(totB) : 0.0;
    const double pE = totE ? 100.0 / static_cast<double>(totE) : 0.0;

    // ⭐ THE LINE THE PHASE ASKED FOR: separate a PERFORMANCE gate from a POLICY gate from a
    // genuinely MISSING GPU authority, weighted by the entries each blocks.
    std::fprintf(stderr,
                 "[G666:discovterm] batches=%llu entries=%llu rows=%zu | widthOnly=%llu/%llu "
                 "(%.2f%%B %.2f%%E) policyOnly=%llu (%.2f%%B %.2f%%E) width+policy=%llu "
                 "(%.2f%%B %.2f%%E) | foreignZ=%llu (%.2f%%B %.2f%%E) depthWrites=%llu "
                 "(%.2f%%B %.2f%%E) | privMirror ro=%llu (%.2f%%E) rw=%llu (%.2f%%E)\n",
                 totB, totE, g_termRows.size(),
                 widthOnlyB, totB, widthOnlyB * pB, widthOnlyE * pE,
                 policyOnlyB, policyOnlyB * pB, policyOnlyE * pE,
                 widthPlusPolicyB, widthPlusPolicyB * pB, widthPlusPolicyE * pE,
                 foreignB, foreignB * pB, foreignE * pE,
                 depthWriteB, depthWriteB * pB, depthWriteE * pE,
                 roPrivB, roPrivE * pE, rwPrivB, rwPrivE * pE);

    std::vector<std::pair<unsigned long long, TermRow>> rows(g_termRows.begin(), g_termRows.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<unsigned long long, TermRow> &a,
                 const std::pair<unsigned long long, TermRow> &b) {
                  return a.second.entries > b.second.entries;
              });
    const size_t cap = std::min<size_t>(rows.size(), 24u);
    for (size_t i = 0; i < cap; ++i)
    {
        const auto &kv = rows[i];
        const unsigned mask = static_cast<unsigned>((kv.first >> 26) & 0x7FFu);
        const unsigned prim = static_cast<unsigned>((kv.first >> 22) & 0x7u);
        const unsigned tme  = static_cast<unsigned>((kv.first >> 21) & 0x1u);
        const unsigned zw   = static_cast<unsigned>((kv.first >> 20) & 0x1u);
        const unsigned gz   = static_cast<unsigned>((kv.first >> 19) & 0x1u);
        const unsigned fbw  = static_cast<unsigned>((kv.first >> 12) & 0x7Fu);
        const unsigned fbp  = static_cast<unsigned>(kv.first & 0x1FFu);
        char terms[192];
        formatTerms(mask, terms, sizeof(terms));
        std::fprintf(stderr,
                     "[G666:discovrow] entries=%llu batches=%llu fbp=%03x fbw=%u prim=%u tme=%u "
                     "guestZ=%u zwrite=%u rows=%d..%d zrows=%d..%d failing=%s\n",
                     kv.second.entries, kv.second.batches, fbp, fbw, prim, tme, gz, zw,
                     kv.second.rowLo, kv.second.rowHi, kv.second.dRowLo, kv.second.dRowHi, terms);
    }
}

// ================================== [G666:zrate] ================================================

bool g666ZRateOn()
{
    static const bool s_on = envOn("DC2_G666_ZRATE");
    return s_on;
}

void g666NoteZRate(unsigned fbp, unsigned prim, unsigned long long rect,
                   unsigned long long cpuWrote, unsigned long long gpuWrote,
                   unsigned long long touched, unsigned long long badTouched,
                   unsigned long long badUntouched, unsigned long long maxDelta,
                   const unsigned long long *hist, int bigX, int bigY,
                   unsigned bigCpu, unsigned bigGpu)
{
    if (!g666ZRateOn())
        return;
    const unsigned key = ((prim & 0xFu) << 12) | (fbp & 0x1FFu);
    {
        std::lock_guard<std::mutex> g(g_zLock);
        ZRow &r = g_zRows[key];
        ++r.batches;
        r.rect += rect;
        r.cpuWrote += cpuWrote;
        r.gpuWrote += gpuWrote;
        r.touched += touched;
        r.badTouched += badTouched;
        r.badUntouched += badUntouched;
        if (maxDelta > r.maxDelta) r.maxDelta = maxDelta;
        if (hist != nullptr)
            for (int i = 0; i < kZHist; ++i)
                r.hist[i] += hist[i];
        // Keep the FIRST large-delta exemplar seen for this (fbp, prim); a later one adds nothing
        // and overwriting would make the row non-deterministic across reporter periods.
        if (r.bigX < 0 && bigX >= 0)
        {
            r.bigX = bigX; r.bigY = bigY; r.bigCpu = bigCpu; r.bigGpu = bigGpu;
        }
    }
    const unsigned long long n = g_zN.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (n <= 8ull || (n % 512ull) == 0ull)
        g666ReportZRate();
}

void g666ReportZRate()
{
    if (!g666ZRateOn())
        return;
    std::lock_guard<std::mutex> g(g_zLock);

    unsigned long long tRect = 0, tTouch = 0, tBadT = 0, tBadU = 0, tB = 0;
    unsigned long long tHist[kZHist] = {0, 0, 0, 0, 0, 0, 0};
    for (const auto &kv : g_zRows)
    {
        tB += kv.second.batches;
        tRect += kv.second.rect;
        tTouch += kv.second.touched;
        tBadT += kv.second.badTouched;
        tBadU += kv.second.badUntouched;
        for (int i = 0; i < kZHist; ++i)
            tHist[i] += kv.second.hist[i];
    }
    // ⭐ BOTH denominators on one line, so the inherited number and the correct one can never be
    // confused again (Rule 20c). `rectRate` is what `zbad` has always been quoted as.
    std::fprintf(stderr,
                 "[G666:zrate] batches=%llu rect=%llu touched=%llu (%.4f%% of rect) | "
                 "badTouched=%llu (%.4f%% of TOUCHED, %.6f%% of rect) | badUntouched=%llu | "
                 "rows=%zu\n",
                 tB, tRect, tTouch,
                 tRect ? 100.0 * static_cast<double>(tTouch) / static_cast<double>(tRect) : 0.0,
                 tBadT,
                 tTouch ? 100.0 * static_cast<double>(tBadT) / static_cast<double>(tTouch) : 0.0,
                 tRect ? 100.0 * static_cast<double>(tBadT) / static_cast<double>(tRect) : 0.0,
                 tBadU, g_zRows.size());

    // ⭐ G667: the shape of the divergence, which the rate and the max together cannot express.
    // `[G655:depthtrace]` showed the FIRST divergent pixel off by exactly 1 LSB while maxDelta is
    // 3,790,057 — a systematic rounding rule and a rare unbounded failure look identical in a rate,
    // and they have OPPOSITE consequences for whether this workload can go GPU-native.
    {
        char line[512];
        int off = 0;
        line[0] = '\0';
        for (int i = 0; i < kZHist; ++i)
            off += std::snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                                 "%s%s=%llu(%.3f%%)", i ? " " : "", kZHistName[i], tHist[i],
                                 tBadT ? 100.0 * static_cast<double>(tHist[i]) /
                                             static_cast<double>(tBadT)
                                       : 0.0);
        std::fprintf(stderr, "[G666:zhist] badTouched=%llu | %s\n", tBadT, line);
    }

    std::vector<std::pair<unsigned, ZRow>> rows(g_zRows.begin(), g_zRows.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<unsigned, ZRow> &a, const std::pair<unsigned, ZRow> &b) {
                  return a.second.touched > b.second.touched;
              });
    for (const auto &kv : rows)
    {
        // `d1Share` is the headline of this row: the fraction of divergences that are a single LSB,
        // i.e. attributable to the float32 gl_FragDepth round trip rather than to a different
        // fragment winning the depth test.
        const unsigned long long big =
            kv.second.hist[4] + kv.second.hist[5] + kv.second.hist[6];
        std::fprintf(stderr,
                     "[G666:zrow] fbp=%03x prim=%u batches=%llu rect=%llu cpuWrote=%llu "
                     "gpuWrote=%llu touched=%llu badTouched=%llu (%.4f%% touched) "
                     "badUntouched=%llu maxDelta=%llu | d1=%llu (%.3f%% of bad) "
                     "ge4k=%llu (%.5f%% of bad) first_big=(%d,%d cpu=%08x gpu=%08x)\n",
                     kv.first & 0x1FFu, (kv.first >> 12) & 0xFu, kv.second.batches,
                     kv.second.rect, kv.second.cpuWrote, kv.second.gpuWrote, kv.second.touched,
                     kv.second.badTouched,
                     kv.second.touched ? 100.0 * static_cast<double>(kv.second.badTouched) /
                                             static_cast<double>(kv.second.touched)
                                       : 0.0,
                     kv.second.badUntouched, kv.second.maxDelta,
                     kv.second.hist[0],
                     kv.second.badTouched ? 100.0 * static_cast<double>(kv.second.hist[0]) /
                                                static_cast<double>(kv.second.badTouched)
                                          : 0.0,
                     big,
                     kv.second.badTouched ? 100.0 * static_cast<double>(big) /
                                                static_cast<double>(kv.second.badTouched)
                                          : 0.0,
                     kv.second.bigX, kv.second.bigY, kv.second.bigCpu, kv.second.bigGpu);
    }
}
