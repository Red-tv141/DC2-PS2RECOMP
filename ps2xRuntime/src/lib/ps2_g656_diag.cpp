// ===== G656 P6/P9: storage + reporters for the upload-edge and bind-loop lap timers ===========
//
// A COLD TU BY CONSTRUCTION (rule 12b), exactly like ps2_g654_layerdiag.cpp. The accumulators, the
// name tables and the printf all live here; the hot TU sees only two out-of-line calls, and in the
// default build not even those (the API include compiles to empty structs).
//
// Compiled ONLY when PS2X_G656_DIAG is on — the file is not in the default source list at all.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

enum
{
    // Must match G656Slot in ps2_g656_diag_api.inc. Kept as a plain count so this TU does not
    // have to include the rasterizer's include web.
    kUploadSlots = 9,
    kBindSlots = 10,
    kSlotCount = kUploadSlots + kBindSlots
};

const char *kUpName[kUploadSlots] = {
    "g289mat", "exec", "g264note", "g278dep", "g261mat",
    "g278disp", "g272mat", "g276disp", "tail"
};

// `exec` is the deferred command graph. It is CONSERVED draw work: the batch would have been
// drained at the next flush edge regardless, so a large `exec` is not a lever (the law is written
// out in the G432 census header in rasterizer_draw_sprite.inc). Everything else on this edge is
// either a GPU->VRAM round trip or pure edge overhead, and both ARE levers.
const bool kUpConserved[kUploadSlots] = {
    false, true, false, false, false, false, false, false, false
};

const char *kBindName[kBindSlots] = {
    "g289src", "g310", "g309", "early", "t8view",
    "g267tri", "g280alias", "producer", "g634raw", "tail"
};

std::atomic<unsigned long long> g_units[kSlotCount];
std::atomic<unsigned long long> g_calls[kSlotCount];

// P6 population census: how many entries EXIT the loop at each stage. A stage that costs cycles
// only because every entry passes through it is a different finding from one that costs cycles
// because a few entries do real work inside it.
std::atomic<unsigned long long> g_bindExit[kBindSlots + 1];
std::atomic<unsigned long long> g_bindEntries;

std::atomic<unsigned long long> g_upEdges;
std::atomic<unsigned long long> g_bindFlushes;

bool envOn(const char *name)
{
    const char *e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

} // namespace

bool g656DiagOn()
{
    static const bool v = envOn("DC2_G656_UPEDGE");
    return v;
}

bool g656BindOn()
{
    static const bool v = envOn("DC2_G656_BINDSPLIT");
    return v;
}

void g656Add(int slot, unsigned long long units, unsigned long long calls)
{
    if (slot < 0 || slot >= kSlotCount)
        return;
    g_units[slot].fetch_add(units, std::memory_order_relaxed);
    g_calls[slot].fetch_add(calls, std::memory_order_relaxed);
}

void g656NoteBindEntry(unsigned exitStage)
{
    g_bindEntries.fetch_add(1ull, std::memory_order_relaxed);
    if (exitStage <= static_cast<unsigned>(kBindSlots))
        g_bindExit[exitStage].fetch_add(1ull, std::memory_order_relaxed);
}

void g656ReportUpload()
{
    const unsigned long long edges = g_upEdges.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((edges % 4096ull) != 0ull)
        return;

    unsigned long long tot = 0ull, conserved = 0ull;
    for (int i = 0; i < kUploadSlots; ++i)
    {
        const unsigned long long v = g_units[i].load(std::memory_order_relaxed);
        tot += v;
        if (kUpConserved[i])
            conserved += v;
    }
    if (tot == 0ull)
        return;

    std::fprintf(stderr, "[G656:upedge] edges=%llu totalMs=%.3f conservedMs=%.3f (%.2f%%) "
                         "deletableMs=%.3f (%.2f%%)\n",
                 edges,
                 static_cast<double>(tot) / 1e6,
                 static_cast<double>(conserved) / 1e6,
                 100.0 * static_cast<double>(conserved) / static_cast<double>(tot),
                 static_cast<double>(tot - conserved) / 1e6,
                 100.0 * static_cast<double>(tot - conserved) / static_cast<double>(tot));
    for (int i = 0; i < kUploadSlots; ++i)
    {
        const unsigned long long v = g_units[i].load(std::memory_order_relaxed);
        const unsigned long long c = g_calls[i].load(std::memory_order_relaxed);
        std::fprintf(stderr, "[G656:upstage] %-9s ms=%10.3f share=%6.2f%% calls=%llu us/call=%.3f %s\n",
                     kUpName[i],
                     static_cast<double>(v) / 1e6,
                     100.0 * static_cast<double>(v) / static_cast<double>(tot),
                     c,
                     (c != 0ull) ? (static_cast<double>(v) / 1e3 / static_cast<double>(c)) : 0.0,
                     kUpConserved[i] ? "CONSERVED" : "");
    }
    std::fflush(stderr);
}

void g656ReportBind()
{
    const unsigned long long fl = g_bindFlushes.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if ((fl % 4096ull) != 0ull)
        return;

    unsigned long long tot = 0ull;
    for (int i = 0; i < kBindSlots; ++i)
        tot += g_units[kUploadSlots + i].load(std::memory_order_relaxed);
    if (tot == 0ull)
        return;

    const unsigned long long ent = g_bindEntries.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[G656:bind] flushes=%llu entries=%llu totalGcyc=%.3f cyc/entry=%.1f\n",
                 fl, ent, static_cast<double>(tot) / 1e9,
                 (ent != 0ull) ? (static_cast<double>(tot) / static_cast<double>(ent)) : 0.0);
    for (int i = 0; i < kBindSlots; ++i)
    {
        const unsigned long long v = g_units[kUploadSlots + i].load(std::memory_order_relaxed);
        const unsigned long long c = g_calls[kUploadSlots + i].load(std::memory_order_relaxed);
        const unsigned long long x = g_bindExit[i].load(std::memory_order_relaxed);
        std::fprintf(stderr, "[G656:bindstage] %-9s Gcyc=%9.3f share=%6.2f%% entered=%llu "
                             "cyc/entered=%.1f exitedHere=%llu\n",
                     kBindName[i],
                     static_cast<double>(v) / 1e9,
                     100.0 * static_cast<double>(v) / static_cast<double>(tot),
                     c,
                     (c != 0ull) ? (static_cast<double>(v) / static_cast<double>(c)) : 0.0,
                     x);
    }
    std::fflush(stderr);
}
