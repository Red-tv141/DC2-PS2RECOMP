// ============================================================================================
// G650 (ROADMAP Priority 6) — CONTENTION-AWARE CORE SCHEDULER
// ============================================================================================
//
// PREMISE. The recompiled runtime runs four hot threads — EE (`GameThread`), the MTVU VU1 worker,
// the GS worker (`GifArbiter::worker`) and the GL backend worker (`lle_gpu_raster_backend`) —
// plus a `GSRowPool` of `hardware_concurrency() - 1` cooperative band-replay lanes and the host
// present loop. On this host that is ~17 OS threads over 12 logical / 6 PHYSICAL cores. Windows
// is free to migrate any of them at any quantum, and two of the four hot threads can land on the
// two SMT siblings of ONE physical core, where they share L1/L2 and the execution ports.
//
// ⚠️ WHY THIS SHIPS AS THREE SEPARATELY-ARMABLE SLICES, NOT ONE "affinity" LEVER.
// G491 measured (law 24) that on a 6-physical-core host with ~5 busy threads, freeing CPU on a
// blocked thread recovers nothing — the freed core had no claimant. The corollary is that HARD
// affinity on this host is not obviously a win: `GSRowPool::runCoop` wants *every* core during a
// dispatch, and a hard mask that reserves cores for threads which are blocked at that instant
// takes lanes away from the pole's own fork/join. So:
//
//   DC2_G650_IDEALCPU=1  — SOFT hint only (`SetThreadIdealProcessor`). The scheduler still may
//                          migrate under load; it just stops doing so for free. Cannot reduce
//                          the pool's parallelism because it reserves nothing.
//   DC2_G650_HARDAFF=1   — HARD mask (`SetThreadAffinityMask`) for the four hot threads, one
//                          PHYSICAL core each (both SMT siblings of that core, so a hot thread
//                          keeps its own L1/L2 and never shares ports with another hot thread).
//                          Implies the ideal-CPU hint. This is the slice that can go negative.
//   DC2_G650_PRIO=1      — THREAD_PRIORITY_ABOVE_NORMAL on the four hot threads only. Deliberately
//                          NOT `TIME_CRITICAL`/`REALTIME_PRIORITY_CLASS`: the pool lanes and the
//                          present loop must stay schedulable, and a starved GL backend lengthens
//                          exactly the blocked half G640 priced.
//
// Each is default-OFF at bring-up and gated on its own A B B A before any promotion.
//
// TOPOLOGY. Physical cores are enumerated from `GetLogicalProcessorInformationEx(RelationProcessorCore)`
// — never inferred from `hardware_concurrency()/2`, which is wrong on any hybrid or non-SMT part.
// Only processor group 0 is used; a >64-logical-CPU host falls back to "unarmed" rather than
// silently pinning everything into group 0.
//
// EXACTNESS. Scheduling hints change no architectural state. There is no oracle and none is
// needed; the gate is the A B B A and the workload invariants that accompany it.
// ============================================================================================

#ifndef DC2_G650_AFFINITY_INC
#define DC2_G650_AFFINITY_INC 1

// ⛔ This file is included from a TU that has ALREADY included <windows.h> (ps2_gif_arbiter.cpp).
// It must never be included into ps2_runtime.cpp: pulling <windows.h> in ahead of raylib.h and
// ThreadNaming.h redefines `Rectangle`, `CloseWindow`, `ShowCursor` and `HMODULE`. Every other TU
// declares `extern void g650PinThread(int role);` at global scope instead.
#include <cstdio>
#include <cstdlib>
#include <vector>

// Roles. Kept as plain ints so the other TUs (`ps2_gif_arbiter.cpp`, the GS/GPU raster TU) can
// call the bridge without a header edit.
//   0 = EE game thread   1 = VU1 (MTVU) worker   2 = GS worker
//   3 = GL backend       4 = host present loop
enum
{
    G650_ROLE_EE = 0,
    G650_ROLE_VU1 = 1,
    G650_ROLE_GS = 2,
    G650_ROLE_GL = 3,
    G650_ROLE_MAIN = 4,
    G650_ROLE_COUNT = 5
};

namespace
{

// ⭐ PROMOTED DEFAULT-ON after the gates below. `DC2_G650_NO_SCHED=1` is the master rollback; the
// three per-slice flags stay accepted as their bring-up spellings, so a probe can still select ONE
// slice by combining them (`DC2_G650_NO_SCHED=1 DC2_G650_HARDAFF=1` = hard affinity only).
//   dragon   HARDAFF        frame −0.775 ms/f (−2.95%)  GS own −0.452  VU1 −0.731   both blocks
//   dungeon1 HARDAFF        frame −0.578 ms/f (−2.28%)  GS own −0.530               both blocks
//   dragon   +PRIO on top   frame −0.361 ms/f (−1.41%)  GS own −0.275  VU1 −0.585   both blocks
bool g650SchedOn()
{
    static const bool s_on = (std::getenv("DC2_G650_NO_SCHED") == nullptr);
    return s_on;
}

bool g650IdealCpuOn()
{
    static const bool s_on = g650SchedOn() || (std::getenv("DC2_G650_IDEALCPU") != nullptr);
    return s_on;
}

bool g650HardAffOn()
{
    static const bool s_on = g650SchedOn() || (std::getenv("DC2_G650_HARDAFF") != nullptr);
    return s_on;
}

bool g650PrioOn()
{
    static const bool s_on = g650SchedOn() || (std::getenv("DC2_G650_PRIO") != nullptr);
    return s_on;
}

bool g650StatOn()
{
    static const bool s_on = (std::getenv("DC2_G650_AFF_STAT") != nullptr);
    return s_on;
}

// ============================================================================================
// ⭐ G651 (ROADMAP Priority 4) — ADAPTIVE CPU TOPOLOGY
//
// PREMISE. G650's map is measured and it ships, but it is HOST-SHAPED (its own Stale-When says so):
// it assumes ≥5 physical cores and treats every core as interchangeable. Two failure modes follow
// on any other machine, and both are SILENT:
//
//   1. ⛔ **N < 5 physical cores.** `g650CoreForRole` ends in `% n`, so on a 4-core part EE and
//      MAIN both land on core 0 and the four hot threads stop having a core each — which is the
//      exact property the −0.775 ms/f came from. Hard-pinning is then strictly worse than letting
//      the OS schedule, so G651 **refuses the hard mask below 5 physical cores** and keeps only
//      the soft hint and the priority bump.
//   2. ⛔ **Hybrid (P/E) parts.** `RelationProcessorCore` enumerates E-cores alongside P-cores in
//      whatever order the firmware reports, so "cores 1..4" can be E-cores while the P-cores sit
//      idle. G651 reads `PROCESSOR_RELATIONSHIP::EfficiencyClass` (Win10 1709+) and, when it is
//      uniformly 0 (older kernels report nothing there), falls back to
//      `GetSystemCpuSetInformation`'s per-CPU `EfficiencyClass`, resolved at run time through
//      `GetProcAddress` so the binary still loads on a host without it.
//
// ⭐ **IDENTITY ON THE MEASURED HOST IS A REQUIREMENT, NOT A NICETY.** On a HOMOGENEOUS host with
// ≥5 physical cores the ranked map is bypassed entirely and G650's static `{MAIN 0, EE 1, VU1 2,
// GS 3, GL 4}` is used verbatim — so `[G650:aff]` prints the same masks it printed for the gates
// (`main 0x3, ee 0xc, vu1 0x30, gs 0xc0, gl 0x300` on the i7-8700) and the shipped A B B A is not
// invalidated by this change. The adaptive path only ever engages where G650 had no measurement.
//
// On a hybrid host the four hot threads take the four highest-`EfficiencyClass` physical cores in
// the order **GS, VU1, EE, GL** (GS or VU1 is the pole on five of the seven surveyed routes, EE on
// the other two but those are already at the display cap), and the present loop takes the LOWEST
// -ranked core. `GSRowPool` lanes stay unpinned in every arm, exactly as G650 left them.
//
// Rollback `DC2_G651_NO_TOPO=1` restores G650's enumeration and static map unconditionally.
// ============================================================================================

bool g651TopoOn()
{
    static const bool s_on = (std::getenv("DC2_G651_NO_TOPO") == nullptr);
    return s_on;
}

// One entry per PHYSICAL core: the affinity mask of every logical CPU on it, the index of its
// first logical CPU (the ideal-processor hint), and (G651) its performance class and SMT width.
struct G650Cores
{
    unsigned count = 0u;
    unsigned long long mask[64] = {};
    unsigned char first[64] = {};
    unsigned char effClass[64] = {}; // higher = more performant (Intel P-core > E-core)
    unsigned char smt[64] = {};      // logical CPUs on this physical core
    unsigned char rank[64] = {};     // physical core index, best-performance first (G651)
    bool hybrid = false;             // at least two distinct effClass values
    bool ok = false;
};

#if defined(_WIN32)
// `GetSystemCpuSetInformation` is Win10 1607+. Resolved dynamically so this binary keeps loading
// on a host (or against an SDK) that does not have it — the fallback is simply "not hybrid".
typedef BOOL(WINAPI *G651GetCpuSetInfoFn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE,
                                          ULONG);

// Fills `effOut[logicalCpu]` from the CPU-set API. Returns false if the API is unavailable.
bool g651CpuSetEfficiency(unsigned char *effOut, unsigned n)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32)
        return false;
    auto fn = reinterpret_cast<G651GetCpuSetInfoFn>(
        reinterpret_cast<void *>(GetProcAddress(k32, "GetSystemCpuSetInformation")));
    if (!fn)
        return false;
    ULONG len = 0;
    fn(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (len == 0)
        return false;
    std::vector<unsigned char> buf(len);
    if (!fn(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), len, &len,
            GetCurrentProcess(), 0))
        return false;
    unsigned char *p = buf.data();
    unsigned char *end = buf.data() + len;
    bool any = false;
    while (p + 8 <= end)
    {
        auto *info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(p);
        if (info->Size == 0 || p + info->Size > end)
            break;
        if (info->Type == CpuSetInformation && info->CpuSet.Group == 0)
        {
            const unsigned lp = info->CpuSet.LogicalProcessorIndex;
            if (lp < n)
            {
                effOut[lp] = info->CpuSet.EfficiencyClass;
                any = true;
            }
        }
        p += info->Size;
    }
    return any;
}
#endif

const G650Cores &g650Cores()
{
    static const G650Cores s_cores = []() -> G650Cores {
        G650Cores c;
#if defined(_WIN32)
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len == 0)
            return c;
        std::vector<unsigned char> buf(len);
        if (!GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
            return c;
        unsigned char *p = buf.data();
        unsigned char *end = buf.data() + len;
        // ⛔ The loop guard must be the ENTRY's own `Size`, never `sizeof(...EX)`. That type is a
        // union whose CACHE/GROUP arms are larger than a RelationProcessorCore entry, so a
        // `p + sizeof(...EX) <= end` guard DROPS THE LAST CORE — measured: an i7-8700 reported
        // `cores=5` instead of 6. Read the 8-byte {Relationship, Size} prefix first, then trust
        // `Size`.
        while (p + 8 <= end && c.count < 64u)
        {
            auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
            if (info->Size == 0 || p + info->Size > end)
                break;
            if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1)
            {
                // Group 0 only — see the header note.
                if (info->Processor.GroupMask[0].Group == 0)
                {
                    const KAFFINITY m = info->Processor.GroupMask[0].Mask;
                    if (m != 0)
                    {
                        unsigned firstBit = 0u;
                        while (firstBit < 64u && ((m >> firstBit) & 1u) == 0u)
                            ++firstBit;
                        unsigned bits = 0u;
                        for (unsigned b = 0u; b < 64u; ++b)
                            bits += static_cast<unsigned>((m >> b) & 1u);
                        c.mask[c.count] = static_cast<unsigned long long>(m);
                        c.first[c.count] = static_cast<unsigned char>(firstBit);
                        c.smt[c.count] = static_cast<unsigned char>(bits);
                        c.effClass[c.count] = info->Processor.EfficiencyClass;
                        ++c.count;
                    }
                }
            }
            p += info->Size;
        }
        c.ok = (c.count >= 2u);

        // ---- G651: performance classes -------------------------------------------------------
        bool anyEff = false;
        for (unsigned i = 0u; i < c.count; ++i)
            if (c.effClass[i] != c.effClass[0])
                anyEff = true;
        if (!anyEff)
        {
            // `RelationProcessorCore::EfficiencyClass` is uniform. Either the part really is
            // homogeneous, or the kernel does not report it — ask the CPU-set API before deciding.
            unsigned char lpEff[64] = {};
            if (g651CpuSetEfficiency(lpEff, 64u))
            {
                for (unsigned i = 0u; i < c.count; ++i)
                {
                    unsigned char best = 0u;
                    for (unsigned b = 0u; b < 64u; ++b)
                        if (((c.mask[i] >> b) & 1ull) && lpEff[b] > best)
                            best = lpEff[b];
                    c.effClass[i] = best;
                }
                for (unsigned i = 0u; i < c.count; ++i)
                    if (c.effClass[i] != c.effClass[0])
                        anyEff = true;
            }
        }
        c.hybrid = anyEff;

        // Rank: performance class DESCENDING, then physical index ascending (stable). On a
        // homogeneous part this is the identity permutation, which is what keeps G650's measured
        // mapping byte-identical here.
        for (unsigned i = 0u; i < c.count; ++i)
            c.rank[i] = static_cast<unsigned char>(i);
        for (unsigned i = 1u; i < c.count; ++i)
        {
            const unsigned char v = c.rank[i];
            unsigned j = i;
            while (j > 0u && c.effClass[c.rank[j - 1u]] < c.effClass[v])
            {
                c.rank[j] = c.rank[j - 1u];
                --j;
            }
            c.rank[j] = v;
        }
#endif
        return c;
    }();
    return s_cores;
}

// G650's static map: the four hot threads take cores 1..4 and the present loop takes core 0.
unsigned g650CoreForRole(int role, unsigned n)
{
    if (n == 0u)
        return 0u;
    static const int kOrder[G650_ROLE_COUNT] = {
        /*EE  */ 1,
        /*VU1 */ 2,
        /*GS  */ 3,
        /*GL  */ 4,
        /*MAIN*/ 0,
    };
    const int idx = (role >= 0 && role < G650_ROLE_COUNT) ? kOrder[role] : 0;
    return static_cast<unsigned>(idx) % n;
}

// ⭐ G651: role -> physical core index, adaptive. On a homogeneous host this RETURNS EXACTLY
// `g650CoreForRole` (the rank permutation is the identity and the rank order below reproduces
// G650's assignment), so the shipped gate stays valid; on a hybrid host the four hot threads take
// the four best-performing physical cores and the present loop takes the worst.
unsigned g651CoreForRole(int role, const G650Cores &c)
{
    const unsigned n = c.count;
    if (n == 0u)
        return 0u;
    if (!g651TopoOn() || !c.hybrid)
        return g650CoreForRole(role, n);

    // Hot-thread order on a hybrid part: the pole first. GS or VU1 is the pole on five of the
    // seven surveyed routes; EE owns the other two, which are already at the display cap.
    static const int kHotRank[G650_ROLE_COUNT] = {
        /*EE  */ 2,
        /*VU1 */ 1,
        /*GS  */ 0,
        /*GL  */ 3,
        /*MAIN*/ -1, // the present loop takes the LAST rank
    };
    const int hr = (role >= 0 && role < G650_ROLE_COUNT) ? kHotRank[role] : -1;
    const unsigned slot = (hr < 0) ? (n - 1u) : (static_cast<unsigned>(hr) % n);
    return c.rank[slot];
}

// ⛔ G651: hard affinity needs one physical core PER hot thread plus one for the pool and the
// present loop. Below that the `% n` wrap co-locates two hot threads, which deletes the very
// property the −0.775 ms/f came from — so the hard mask is refused and only the soft hint and the
// priority bump remain.
bool g651HardAffAdmissible(const G650Cores &c)
{
    if (!g651TopoOn())
        return true;
    return c.count >= 5u;
}

const char *g650RoleName(int role)
{
    switch (role)
    {
    case G650_ROLE_EE: return "ee";
    case G650_ROLE_VU1: return "vu1";
    case G650_ROLE_GS: return "gs";
    case G650_ROLE_GL: return "gl";
    case G650_ROLE_MAIN: return "main";
    default: return "?";
    }
}

} // namespace

// GLOBAL linkage on purpose: `ps2_gif_arbiter.cpp` and the GS/GPU raster TU call this from their
// own worker entry points and cannot see this anonymous namespace (the appendix's anon-namespace
// linkage trap). Declared `extern void g650PinThread(int);` at global scope in those TUs.
void g650PinThread(int role)
{
#if defined(_WIN32)
    const bool ideal = g650IdealCpuOn();
    const bool hard = g650HardAffOn();
    const bool prio = g650PrioOn();
    if (!ideal && !hard && !prio)
        return;

    const HANDLE self = GetCurrentThread();
    unsigned coreIdx = 0u;
    unsigned long long usedMask = 0ull;
    unsigned char idealCpu = 0u;
    bool hardApplied = false;

    const G650Cores &c = g650Cores();
    if ((ideal || hard) && c.ok)
    {
        coreIdx = g651CoreForRole(role, c);
        usedMask = c.mask[coreIdx];
        idealCpu = c.first[coreIdx];
        if (hard && usedMask != 0ull && g651HardAffAdmissible(c))
        {
            SetThreadAffinityMask(self, static_cast<DWORD_PTR>(usedMask));
            hardApplied = true;
        }
        // The ideal-CPU hint is set in BOTH arms: under `hard` it tells the scheduler which
        // of the core's two SMT siblings to prefer, which the mask alone does not.
        SetThreadIdealProcessor(self, static_cast<DWORD>(idealCpu));
    }

    if (prio)
        SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL);

    if (g650StatOn())
    {
        std::fprintf(stderr,
                     "[G650:aff] role=%s ideal=%u hard=%u prio=%u core=%u mask=0x%llx idealCpu=%u cores=%u\n",
                     g650RoleName(role), ideal ? 1u : 0u, hardApplied ? 1u : 0u, prio ? 1u : 0u,
                     coreIdx, usedMask, static_cast<unsigned>(idealCpu), c.count);
        // ⭐ G651: one line naming what the host actually is, so a bad map on an unknown machine is
        // visible in the log instead of being inferred from a frame number.
        static bool s_topoPrinted = false;
        if (!s_topoPrinted)
        {
            s_topoPrinted = true;
            std::fprintf(stderr,
                         "[G651:topo] cores=%u hybrid=%u topoOn=%u hardAdmissible=%u rank=",
                         c.count, c.hybrid ? 1u : 0u, g651TopoOn() ? 1u : 0u,
                         g651HardAffAdmissible(c) ? 1u : 0u);
            for (unsigned i = 0u; i < c.count; ++i)
                std::fprintf(stderr, "%s%u(eff=%u,smt=%u)", i ? "," : "", (unsigned)c.rank[i],
                             (unsigned)c.effClass[c.rank[i]], (unsigned)c.smt[c.rank[i]]);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    }
#else
    (void)role;
#endif
}

#endif // DC2_G650_AFFINITY_INC
