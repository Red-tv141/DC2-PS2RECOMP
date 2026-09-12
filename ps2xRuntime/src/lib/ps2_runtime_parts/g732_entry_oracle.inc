// ===== G732: THE ENTRY-POINT ORACLE ==========================================================
//
// G732 removes the ps2xRecomp indirect-jump FALLBACK for `jalr`. A JALR is an indirect CALL: it
// leaves the function and its only in-function continuation is the instruction after its delay
// slot, which queueResumeEntryTarget() already registers exactly. Classifying it as a computed
// JUMP made collectInternalBranchTargets() mark EVERY instruction of the containing function an
// entry point -- a label, an entry-switch case, and a runtime registerFunction() call each.
//
// Measured cost of that misclassification: 714 generated files carrying 125,545 entry-switch
// cases, 119,215 labels, 28,102 delay-slot re-entry prologues and 124,831 mid-function
// registrations; -37% host instructions on an affected body once removed; and the EE sampler's
// top generated-code symbol on `dungeon1` (CreateFacePacket, 7.65% of the EE thread) is one of
// the functions it applies to.
//
// ⛔ THE ONE THING THAT MUST BE PROVEN, NOT ARGUED. Dropping those 124,831 registrations is only
// safe if nothing ever dispatches to a mid-function address that loses its registration. The
// static argument is strong -- functions containing a genuine computed `jr` keep the full
// fallback, cross-function static entries still go through queueExternalEntryTarget(), call
// returns are registered exactly, and MIPS $t9 holds a function ENTRY by ABI -- but this project
// does not ship an argument where it can ship a count.
//
// MECHANISM. v1 inferred the doomed set as "registered against a body whose entry address is
// lower". ⛔ ITS OWN SELF-CHECK REFUTED THAT: it found 175,711 such addresses against the 124,831
// the corpus diff actually removes, because plenty of mid-function addresses are registered for
// reasons G732 KEEPS -- externalEntryPoints, and the cross-function resume targets in
// m_resumeEntryTargetsByOwner. Counting hits on the wider set would have condemned the change for
// traffic it does not affect.
//
// v2 therefore tests the EXACT set, extracted from the register_functions.cpp diff and loaded from
// the file named by DC2_G732_REMOVED_LIST (one 0x-prefixed address per line). An address counts
// only if G732 genuinely unregisters it.
//
// SELF-CHECK (Rule 69/90 -- an instrument must be shown to see the right population): the loaded
// set size must read exactly 124,831, and the report prints it, so an oracle pointed at the wrong
// file or an empty one cannot read as a clean pass.
//
// Diagnostic only. Env-gated, and built into a TAGGED binary, never the deliverable (Rule 31:
// inert code in a hot TU is not free).
#ifndef PS2_G732_ENTRY_ORACLE_INC
#define PS2_G732_ENTRY_ORACLE_INC

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// File scope, not a function-local static: read on every indirect dispatch, where an MSVC
// magic-static guard is itself measurable (G619's law).
static const bool g_g732EntryOracleOn = (std::getenv("DC2_G732_ENTRY_ORACLE") != nullptr);

static std::unordered_map<void *, uint32_t> g_g732FnEntry;   // function pointer -> its entry address
static std::unordered_set<uint32_t> g_g732NonEntry;          // v1 inferred set, kept as cross-check
static std::unordered_set<uint32_t> g_g732Removed;           // v2 EXACT set: what G732 unregisters
static std::atomic<uint64_t> g_g732Lookups{0ull};
static std::atomic<uint64_t> g_g732NonEntryHits{0ull};       // hits on the v1 inferred set
static std::atomic<uint64_t> g_g732RemovedHits{0ull};        // ⭐ hits on the EXACT removed set
static std::unordered_map<uint32_t, uint64_t> g_g732HitHist;
static std::mutex g_g732HitMutex;

// Load the exact removed-address list once, at arm time.
static const bool g_g732RemovedLoaded = []() -> bool {
    const char *path = std::getenv("DC2_G732_REMOVED_LIST");
    if (!path)
        return false;
    std::FILE *f = std::fopen(path, "r");
    if (!f)
        return false;
    char line[64];
    while (std::fgets(line, sizeof(line), f))
    {
        unsigned long v = std::strtoul(line, nullptr, 0);
        if (v)
            g_g732Removed.insert(static_cast<uint32_t>(v));
    }
    std::fclose(f);
    return true;
}();

// Called from registerFunction(). Builds the v1 inferred set as the table is populated.
static inline void g732NoteRegistration(uint32_t address, void *fn)
{
    if (!g_g732EntryOracleOn || fn == nullptr)
        return;
    auto it = g_g732FnEntry.find(fn);
    if (it == g_g732FnEntry.end())
    {
        g_g732FnEntry.emplace(fn, address); // first address seen for this body == its entry
        return;
    }
    if (address == it->second)
        return;
    if (address < it->second)
    {
        // Registration order was not ascending after all: the previous "entry" was a mid-address.
        g_g732NonEntry.insert(it->second);
        it->second = address;
        return;
    }
    g_g732NonEntry.insert(address);
}

// Called from lookupFunction() / hasFunction().
static void g732EntryOracleReport();

static inline void g732NoteLookup(uint32_t address)
{
    if (!g_g732EntryOracleOn)
        return;
    const uint64_t seen = g_g732Lookups.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
    if (g_g732NonEntry.find(address) != g_g732NonEntry.end())
        g_g732NonEntryHits.fetch_add(1ull, std::memory_order_relaxed);
    // ⭐ the number that decides the phase: a hit on an address G732 actually unregisters.
    if (g_g732Removed.find(address) != g_g732Removed.end())
    {
        g_g732RemovedHits.fetch_add(1ull, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_g732HitMutex);
        ++g_g732HitHist[address];
    }
    // Periodic report: this is a soak oracle, so it must produce a reading even if the route
    // is killed rather than exited cleanly.
    if ((seen % 20000000ull) == 0ull)
        g732EntryOracleReport();
}

static void g732EntryOracleReport()
{
    if (!g_g732EntryOracleOn)
        return;
    const uint64_t look = g_g732Lookups.load(std::memory_order_relaxed);
    const uint64_t wide = g_g732NonEntryHits.load(std::memory_order_relaxed);
    const uint64_t bad = g_g732RemovedHits.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G732:entry] removedSet=%zu (expect 124831, loaded=%d) lookups=%llu "
                 "REMOVEDHITS=%llu | inferredSet=%zu inferredHits=%llu bodies=%zu\n",
                 g_g732Removed.size(), g_g732RemovedLoaded ? 1 : 0,
                 static_cast<unsigned long long>(look),
                 static_cast<unsigned long long>(bad),
                 g_g732NonEntry.size(), static_cast<unsigned long long>(wide),
                 g_g732FnEntry.size());
    if (bad == 0ull)
        return;
    std::lock_guard<std::mutex> lk(g_g732HitMutex);
    std::vector<std::pair<uint32_t, uint64_t>> v(g_g732HitHist.begin(), g_g732HitHist.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < v.size() && i < 20; ++i)
        std::fprintf(stderr, "[G732:entry]   pc=0x%08x n=%llu\n",
                     v[i].first, static_cast<unsigned long long>(v[i].second));
}

#endif // PS2_G732_ENTRY_ORACLE_INC
