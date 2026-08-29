// ===== G663: storage + reporter for the bind-pre-pass SCAN census ============================
//
// A COLD TU BY CONSTRUCTION (rule 12b), exactly like ps2_g654_layerdiag.cpp and ps2_g662_diag.cpp.
// The accumulators, the histogram and the printf all live here; the hot TU sees only one
// out-of-line call, and in the default build not even that (the call site is `#if`-excluded).
//
// Compiled ONLY when PS2X_G663_DIAG is on - the file is not in the default source list at all.
//
// WHAT IT ANSWERS. `[G323:census]` prices the bind pre-pass at 2.838 ms/f on `dragon:tail` and
// 3.754 ms/f on `s03:steady` - the largest convertible CPU term in the whole `exec` partition.
// `[G656:bindstage]` then puts 56-61% of that in the `tail`, whose dominant term is
//
//     for (uint32_t t = 0; t < kG248TargetCount; ++t)   // 14 slots, PER ENTRY
//         ...
//         if (!g_g261Res[t].dirty) continue;
//
// i.e. 18.3 M entries x 14 = 256 M iterations per `dragon` run, most of which exist only to
// discover that a residency slot is clean. The hoist is exact (nothing inside this loop can SET
// `dirty` - `g282CommitFanout` is its only setter and its only caller runs after the pre-pass),
// but "exact" is not "worth it": if most slots were dirty most of the time the hoist would save
// nothing. THIS COUNTS THE POPULATION so the ceiling is known before the timing arm is believed.
//
// Reporting obeys G657 measurement law 10 / G661 section 1.6b rule 3: the denominator is counted
// unconditionally, the report is keyed to it, and it prints at n == 1 as well as periodically, so
// "no line" can never be confused with "zero".

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace
{

enum
{
    kTargets = 14 // must match kG248TargetCount (rasterizer_tilebinning_and_probes.inc)
};

std::atomic<unsigned long long> g_flushes{0};
std::atomic<unsigned long long> g_entries{0};   // entries that REACHED the per-target scan
std::atomic<unsigned long long> g_scanFull{0};  // iterations the unhoisted loop would run
std::atomic<unsigned long long> g_scanDirty{0}; // iterations the hoisted loop actually runs
// Per-flush count of dirty residency slots, bucketed 0..14.
std::atomic<unsigned long long> g_hist[kTargets + 1];
std::atomic<unsigned long long> g_armed{0};

// ===== [G663:tailsplit] - WHICH HALF OF `tail` ===============================================
// `[G656:bindstage]` put 56-61% of the bind loop in `tail` and G663 assumed that meant the
// 14-slot residency scan. The hoist that removes 92.86% of those iterations measured a NULL
// (s03:steady -0.100 ms/f, blocks +0.042/-0.242, mixed sign), so the assumption was wrong.
// `tail` has TWO halves and G656's lap table could not separate them:
//   0 texpub  the `if (e.prim.tme)` texture-publication fallback - g633 plan lookup,
//             g144TextureRange, g632NoteTexReject and g630MaterializeForRanges. This is a real
//             CPU<->GPU ownership transfer, and unlike the g261Materialize at :3595 it is NOT
//             excluded from `[G323:census] bind`.
//   1 tgtscan the per-entry walk of the residency slots.
// Two slots, two rdtsc pairs, diagnostic build only.
enum { kTailSlots = 2 };
const char *kTailName[kTailSlots] = { "texpub", "tgtscan" };
std::atomic<unsigned long long> g_tailCyc[kTailSlots];
std::atomic<unsigned long long> g_tailN[kTailSlots];
std::atomic<unsigned long long> g_tailReports{0};

} // namespace

bool g663DiagOn()
{
    static const bool v = [] {
        const char *e = std::getenv("DC2_G663_BINDSCAN_STAT");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return v;
}

// Called once per flush that runs the bind pre-pass, from the hot TU.
//   dirtyN  - how many of the 14 residency slots were dirty when the pre-pass began
//   entries - how many entries reached the per-target scan in this flush
//   armed   - whether the hoisted iteration was actually selected
void g663NoteBindScan(unsigned dirtyN, unsigned long long entries, bool armed)
{
    if (dirtyN > kTargets)
        dirtyN = kTargets;
    const unsigned long long n = g_flushes.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    g_entries.fetch_add(entries, std::memory_order_relaxed);
    g_scanFull.fetch_add(entries * static_cast<unsigned long long>(kTargets),
                         std::memory_order_relaxed);
    g_scanDirty.fetch_add(entries * static_cast<unsigned long long>(dirtyN),
                          std::memory_order_relaxed);
    g_hist[dirtyN].fetch_add(1ull, std::memory_order_relaxed);
    if (armed)
        g_armed.fetch_add(1ull, std::memory_order_relaxed);

    // Keyed to the DENOMINATOR, and printed at n == 1 as well as periodically.
    if (n != 1ull && (n % 4096ull) != 0ull)
        return;

    const unsigned long long ent = g_entries.load(std::memory_order_relaxed);
    const unsigned long long full = g_scanFull.load(std::memory_order_relaxed);
    const unsigned long long dirty = g_scanDirty.load(std::memory_order_relaxed);
    char buf[768];
    int off = std::snprintf(
        buf, sizeof(buf),
        "[G663:bindscan] flushes=%llu armedFlushes=%llu scanEntries=%llu "
        "iters(full=%llu hoisted=%llu saved=%.2f%%) dirtyPerFlush",
        n, g_armed.load(std::memory_order_relaxed), ent, full, dirty,
        full != 0ull ? 100.0 * static_cast<double>(full - dirty) / static_cast<double>(full) : 0.0);
    for (int i = 0; i <= kTargets && off > 0 && off < static_cast<int>(sizeof(buf)); ++i)
    {
        const unsigned long long v = g_hist[i].load(std::memory_order_relaxed);
        if (v == 0ull)
            continue;
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             " %d:%llu(%.1f%%)", i, v,
                             100.0 * static_cast<double>(v) / static_cast<double>(n));
    }
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
}

// Called once per entry that reaches the tail of the bind pre-pass, from the hot TU.
void g663NoteTail(unsigned long long texpubCyc, unsigned long long scanCyc)
{
    g_tailCyc[0].fetch_add(texpubCyc, std::memory_order_relaxed);
    g_tailCyc[1].fetch_add(scanCyc, std::memory_order_relaxed);
    g_tailN[0].fetch_add(1ull, std::memory_order_relaxed);
    g_tailN[1].fetch_add(1ull, std::memory_order_relaxed);
    const unsigned long long n =
        g_tailReports.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    // Keyed to the denominator; prints at n == 1 as well as periodically.
    if (n != 1ull && (n % 4194304ull) != 0ull)
        return;
    unsigned long long tot = 0ull;
    for (int i = 0; i < kTailSlots; ++i)
        tot += g_tailCyc[i].load(std::memory_order_relaxed);
    char buf[512];
    int off = std::snprintf(buf, sizeof(buf), "[G663:tailsplit] tailEntries=%llu totalGcyc=%.3f",
                            n, static_cast<double>(tot) / 1e9);
    for (int i = 0; i < kTailSlots && off > 0 && off < static_cast<int>(sizeof(buf)); ++i)
    {
        const unsigned long long v = g_tailCyc[i].load(std::memory_order_relaxed);
        const unsigned long long c = g_tailN[i].load(std::memory_order_relaxed);
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             " %s(Gcyc=%.3f share=%.2f%% cyc/entry=%.1f)", kTailName[i],
                             static_cast<double>(v) / 1e9,
                             tot != 0ull ? 100.0 * static_cast<double>(v) /
                                               static_cast<double>(tot) : 0.0,
                             c != 0ull ? static_cast<double>(v) / static_cast<double>(c) : 0.0);
    }
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
}
