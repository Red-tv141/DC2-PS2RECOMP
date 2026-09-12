// ==============================================================================================
// G737 — (OUT-OF-EXE LEAF × OUR CALL SITE) CROSS-TAB FOR THE G446 PC SAMPLER
// ==============================================================================================
//
// ⛔ THE DEFECT THIS CLOSES. `[G446:*]` prints TWO independent one-dimensional histograms for a
// thread that spends 78–84 % of its samples outside our image:
//
//     [G446:execprof]        6.52% n=488  NtQueryInformationThread+0x0      <- the LEAF
//     [G446:execprofcaller] 31.44% n=2353 g713RunNode+0x2b                  <- SOME call site
//
// The caller table is taken over EVERY out-of-exe sample, so it cannot say which caller produced
// which leaf. On the G713 executor that is the whole question: `NtWaitForAlertByThreadId` (a park)
// and `NtQueryInformationThread` (a non-blocking query, so it is COST, not wait) both land in the
// same undifferentiated 84 %, and the top caller row is a park site that explains only the first.
// A reader pairing row 1 of one table with row 1 of the other is guessing, and G733 already proved
// that guess wrong once (`Alloc__9mgCMemory` at 20.24 % against a 20.87 % wait leaf — a
// near-perfect join, and false).
//
// WHAT THIS ADDS. One joint table keyed on (leaf, callSite), recorded from the same sample the two
// existing tables are recorded from, so the three are consistent by construction. The caller is the
// G733 RtlVirtualUnwind frame when that is armed and found one, and the G503 stack scan otherwise;
// the row says which, because their error modes differ (Rule: the unwinder cannot walk into a dead
// frame, the scan can).
//
// ⭐ WHY A WAIT LEAF AND A QUERY LEAF MUST NOT SHARE A BUCKET. `ofWhichKernelWait` is
// "rip outside the exe" (G736 §1 correction), which on a GL-owning thread also counts the driver's
// own CPU work and every non-blocking syscall. Splitting by leaf is the only way to tell a thread
// that is PARKED from a thread that is BUSY inside somebody else's code, and those two have
// opposite consequences for the frame.
//
// COST AND PLACEMENT. Armed by DC2_G737_PAIRS=1; the note is one predicate when disarmed. It runs
// on the SAMPLER thread (~1 kHz), never on the sampled thread, so it perturbs nothing that is
// measured. It lives in THIS translation unit — the cold G713 pipeline TU — and not beside the
// sampler, because `g446_gsworker_pcsample.inc` is included into `ps2_gif_arbiter.cpp`, which
// holds `processGIFPacket`; G710 measured **+0.362 ms/f for compile-time-DEAD text in a hot TU**,
// so the sampler's TU gets four lines of extern call and nothing else.
//
// Symbolisation follows G736's law: a name is a hypothesis, so every row also prints the caller's
// PREFERRED-BASE rva for `tools/g736_resolve_pcsample.py`, and the leaf's claimed extent.
// ==============================================================================================

#if defined(_WIN32)

#include <algorithm> // std::sort — the host TU does not include it for its own needs
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{

constexpr size_t kG737PairSlots = 16384; // open-addressed (leaf,caller) -> count, power of two

struct G737Pair
{
    uint64_t leaf;
    uint64_t caller;
    uint64_t count;
    uint32_t viaUnwind;
};

G737Pair g_g737Pairs[kG737PairSlots]{};
uint64_t g_g737Recorded = 0;
uint64_t g_g737Overflow = 0;
uint64_t g_g737ViaUnwind = 0;
uint64_t g_g737ViaScan = 0;

inline uint64_t g737Mix(uint64_t a, uint64_t b)
{
    uint64_t h = a * 0x9E3779B97F4A7C15ull;
    h ^= b + 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

// G736 rule, applied here too: DbgHelp returns the NEAREST PRECEDING symbol, so a match whose
// claimed extent cannot cover the displacement is a FOLD onto a neighbour and must be rejected —
// otherwise the leaf keeps a name that belongs to a different function and the bucket merges two
// unrelated leaves. `size` is reported so the fold is falsifiable.
uint64_t g737LeafBase(uint64_t rip, uint64_t *sizeOut)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    std::memset(sym, 0, sizeof(SYMBOL_INFO));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 500;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym) == FALSE)
    {
        if (sizeOut != nullptr)
            *sizeOut = 0ull;
        return rip;
    }
    if (sizeOut != nullptr)
        *sizeOut = sym->Size;
    if (sym->Size != 0ull && disp >= sym->Size)
        return rip; // fold rejected: keep this PC's own identity
    return sym->Address;
}

void g737SymbolName(uint64_t addr, char *out, size_t cap)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    std::memset(sym, 0, sizeof(SYMBOL_INFO));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 500;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym) != FALSE)
        std::snprintf(out, cap, "%s+0x%llx", sym->Name, static_cast<unsigned long long>(disp));
    else
        std::snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(addr));
}

} // namespace

bool g737PairsOn()
{
    static const bool on = (std::getenv("DC2_G737_PAIRS") != nullptr);
    return on;
}

// Called from the sampler loop for every out-of-exe sample. `unwCaller` is the G733 unwinder's
// answer (0 when it did not run or found nothing); `scanCaller` is the G503 stack scan's.
void g737NotePair(uint64_t leafRip, uint64_t unwCaller, uint64_t scanCaller)
{
    if (!g737PairsOn())
        return;
    const bool viaUnwind = (unwCaller != 0ull);
    const uint64_t caller = viaUnwind ? unwCaller : scanCaller;
    if (caller == 0ull)
        return;
    // Bucket the leaf at 32-byte granularity on the RECORDING path so ntdll's fixed-size syscall
    // stubs collapse to one row without a SymFromAddr call per sample (which would take the DbgHelp
    // lock 1000x/s). Report time does the exact, extent-checked fold.
    const uint64_t leafKey = leafRip & ~31ull;
    ++g_g737Recorded;
    if (viaUnwind)
        ++g_g737ViaUnwind;
    else
        ++g_g737ViaScan;
    size_t idx = static_cast<size_t>(g737Mix(leafKey, caller) & (kG737PairSlots - 1u));
    for (size_t probe = 0; probe < kG737PairSlots; ++probe)
    {
        G737Pair &p = g_g737Pairs[idx];
        if (p.count == 0ull)
        {
            p.leaf = leafKey;
            p.caller = caller;
            p.count = 1ull;
            p.viaUnwind = viaUnwind ? 1u : 0u;
            return;
        }
        if (p.leaf == leafKey && p.caller == caller)
        {
            ++p.count;
            if (viaUnwind)
                p.viaUnwind = 1u;
            return;
        }
        idx = (idx + 1u) & (kG737PairSlots - 1u);
    }
    ++g_g737Overflow;
}

// Called from g446Report(). `denom` is the sampler's own total sample count, so the percentages in
// this table are directly comparable with the two one-dimensional tables above it.
void g737ReportPairs(const char *label, double denom, uint64_t exeLo, uint64_t exeHi)
{
    if (!g737PairsOn() || g_g737Recorded == 0ull || denom <= 0.0)
        return;

    constexpr size_t kTop = 40;
    static G737Pair top[kTop];
    size_t nTop = 0;
    for (size_t i = 0; i < kG737PairSlots; ++i)
    {
        const G737Pair &p = g_g737Pairs[i];
        if (p.count == 0ull)
            continue;
        if (nTop < kTop)
        {
            top[nTop++] = p;
        }
        else
        {
            size_t worst = 0;
            for (size_t j = 1; j < kTop; ++j)
                if (top[j].count < top[worst].count)
                    worst = j;
            if (top[worst].count < p.count)
                top[worst] = p;
        }
    }
    std::sort(top, top + nTop,
              [](const G737Pair &a, const G737Pair &b) { return a.count > b.count; });

    std::fprintf(stderr,
                 "[G737:%spair] recorded=%llu viaUnwind=%llu viaScan=%llu overflow=%llu "
                 "(leaf x OUR call site; caller is the G733 unwind frame when armed, else the "
                 "G503 stack scan)\n",
                 label,
                 static_cast<unsigned long long>(g_g737Recorded),
                 static_cast<unsigned long long>(g_g737ViaUnwind),
                 static_cast<unsigned long long>(g_g737ViaScan),
                 static_cast<unsigned long long>(g_g737Overflow));
    for (size_t i = 0; i < nTop; ++i)
    {
        uint64_t leafSize = 0ull;
        const uint64_t leafBase = g737LeafBase(top[i].leaf, &leafSize);
        char leafName[560];
        char callName[560];
        g737SymbolName(leafBase, leafName, sizeof(leafName));
        g737SymbolName(top[i].caller, callName, sizeof(callName));
        const uint64_t pref = (exeLo != 0ull && top[i].caller >= exeLo && top[i].caller < exeHi)
                                  ? top[i].caller - exeLo + 0x140000000ull
                                  : 0ull;
        std::fprintf(stderr,
                     "[G737:%spair]   %5.2f%% n=%llu via=%s leafSz=%llu  %s  <=  callPref=0x%llx %s\n",
                     label,
                     100.0 * static_cast<double>(top[i].count) / denom,
                     static_cast<unsigned long long>(top[i].count),
                     top[i].viaUnwind ? "unw" : "scan",
                     static_cast<unsigned long long>(leafSize),
                     leafName,
                     static_cast<unsigned long long>(pref),
                     callName);
    }
}

#else  // !_WIN32

bool g737PairsOn() { return false; }
void g737NotePair(uint64_t, uint64_t, uint64_t) {}
void g737ReportPairs(const char *, double, uint64_t, uint64_t) {}

#endif // _WIN32
