// ===== G446: host PC sampler for the GS worker thread (diagnosis only, default OFF) =====
//
// PREMISE. G445 took the GL backend from 24.7 to 9.4 ms/f and left `[G332:gsw] frontMs/f = 22.2`
// as the binding term of the frame (GS sensitivity 0.97-0.98, i.e. the frame IS this thread's
// span). No phase has ever split that 22 ms: G434 and G444 both priced it whole and from the
// OUTSIDE, and `[G316:worker]` only splits it by GIF PATH (p1/p2/p3), not by the host code that
// runs inside a path. `[G171:regstat]` splits it by GS REGISTER, but its per-call numbers for the
// cheap registers are dominated by its own two `steady_clock::now()` calls (~20 ns each), so it
// cannot rank anything under ~50 ns/call and it cannot see work that is not a register write at
// all (tile-bin, sort, deswizzle, allocation, the l2l path).
//
// A statistical PC sampler has none of those blind spots and costs the sampled thread only the
// suspend/resume pair. It is the instrument G183 is for the EE (guest PC), one level down: this
// samples the HOST rip of the GS worker and resolves it against the runner's PDB, which the build
// already emits with public (function-level) symbols (`/DEBUG`, see the root CMakeLists G377 note)
// and which the G376 crash reporter already resolves with the same DbgHelp calls.
//
// Behaviour: none. Every hook is a no-op unless DC2_G446_GSPROF=1. When armed, one extra thread
// runs at ~1 kHz; the sampled thread pays a SuspendThread/GetThreadContext/ResumeThread round trip
// per sample (~5-10 us of wall time per second of run, i.e. ~1% of one frame) which is charged to
// BOTH arms of nothing, since this instrument is never used inside an A/B. Absolute frame numbers
// taken WITH it armed are not admissible; only the sample DISTRIBUTION is.
//
// The sampler never allocates while the target is suspended (fixed open-addressed table), so it
// cannot deadlock against the CRT heap lock the suspended thread may hold.

#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

// G446 env-read census (diagnosis only, default OFF, DC2_G446_ENVCENSUS=1). The PC sampler above
// measured getenv() at 1.4% of ALL GS-worker samples on a thread whose span IS the frame, but a PC
// sampler cannot name the CALLER. Every envFlagEnabled/envFlagDisabled call site passes a distinct
// string LITERAL, so bucketing by the name pointer names the defective predicate exactly. Global
// linkage because the callers live in three TUs' anonymous namespaces.
void g446NoteEnvRead(const char *name)
{
    static const bool s_on = (std::getenv("DC2_G446_ENVCENSUS") != nullptr);
    if (!s_on)
        return;
    struct Rec { const char *name; std::atomic<uint64_t> n; };
    static Rec s_recs[64]{};
    static std::atomic<uint32_t> s_used{0};
    static std::atomic<uint64_t> s_total{0};
    uint32_t used = s_used.load(std::memory_order_acquire);
    uint32_t idx = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < used && i < 64u; ++i)
        if (s_recs[i].name == name) { idx = i; break; }
    if (idx == 0xFFFFFFFFu)
    {
        idx = s_used.fetch_add(1u, std::memory_order_acq_rel);
        if (idx >= 64u)
            return;
        s_recs[idx].name = name;
    }
    s_recs[idx].n.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t t = s_total.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((t % 250000ull) == 0ull)
    {
        used = s_used.load(std::memory_order_acquire);
        if (used > 64u) used = 64u;
        std::fprintf(stderr, "[G446:envcensus] totalEnvReads=%llu distinct=%u\n",
                     static_cast<unsigned long long>(t), used);
        for (uint32_t i = 0; i < used; ++i)
        {
            const uint64_t n = s_recs[i].n.load(std::memory_order_relaxed);
            if (n * 200ull >= t) // >= 0.5% of all reads
                std::fprintf(stderr, "[G446:envcensus]   %6.2f%% n=%llu %s\n",
                             100.0 * static_cast<double>(n) / static_cast<double>(t),
                             static_cast<unsigned long long>(n), s_recs[i].name);
        }
    }
}

namespace
{
    bool g446GsProfOn()
    {
#ifdef _WIN32
        static const bool s_on = (std::getenv("DC2_G446_GSPROF") != nullptr);
        return s_on;
#else
        return false;
#endif
    }

#ifdef _WIN32
    // Sample period in microseconds (DC2_G446_GSPROF_US, default 1000 = ~1 kHz).
    unsigned g446SampleUs()
    {
        static const unsigned s_us = [] {
            const char *v = std::getenv("DC2_G446_GSPROF_US");
            const unsigned u = (v != nullptr) ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
            return (u >= 50u && u <= 100000u) ? u : 1000u;
        }();
        return s_us;
    }

    // Report period in seconds (DC2_G446_GSPROF_SEC, default 15).
    unsigned g446ReportSec()
    {
        static const unsigned s_s = [] {
            const char *v = std::getenv("DC2_G446_GSPROF_SEC");
            const unsigned u = (v != nullptr) ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
            return (u >= 2u && u <= 600u) ? u : 15u;
        }();
        return s_s;
    }

    constexpr size_t kG446Slots = 8192;   // open-addressed rip -> count, power of two
    struct G446Bucket
    {
        uint64_t rip;
        uint64_t count;
    };
    G446Bucket g_g446Table[kG446Slots]{};
    uint64_t g_g446Total = 0;
    uint64_t g_g446Failed = 0;
    uint64_t g_g446Distinct = 0;

    // Cross-tab every sample by the worker's own loop-stage breadcrumb (G189's
    // g_g189WorkerStage, which the worker loop already stores unconditionally):
    // 0 = idle at the work queue (the cv wait, i.e. NOT the frame's critical path),
    // 1 = popped window, 2 = running window, 3 = popped boundary / enter-flush,
    // 4 = enter-latch, 5 = boundary-done, 6 = apply.
    // Without this a PC sampler cannot tell "blocked because there is nothing to do" from
    // "blocked inside the work", and those two have opposite consequences for the frame.
    std::atomic<int> *g_g446Stage = nullptr;
    uint64_t g_g446StageCount[8]{};
    uint64_t g_g446StageWait[8]{}; // of which: sampled inside a kernel wait (rip outside the exe)
    uint64_t g_g446ExeLo = 0, g_g446ExeHi = 0;
    const char *g446StageName(int s)
    {
        switch (s)
        {
        case 0: return "idle-at-queue";
        case 1: return "window-popped";
        case 2: return "window-run";
        case 3: return "boundary-flush";
        case 4: return "boundary-latch";
        case 5: return "boundary-done";
        case 6: return "apply";
        default: return "?";
        }
    }

    void g446Record(uint64_t rip)
    {
        // FNV-ish mix; rip is 16-byte-ish sparse so shift out the low bits' correlation.
        uint64_t h = rip * 0x9E3779B97F4A7C15ull;
        size_t idx = static_cast<size_t>(h >> 51) & (kG446Slots - 1);
        for (size_t probe = 0; probe < kG446Slots; ++probe)
        {
            G446Bucket &b = g_g446Table[idx];
            if (b.rip == rip)
            {
                ++b.count;
                return;
            }
            if (b.count == 0)
            {
                b.rip = rip;
                b.count = 1;
                ++g_g446Distinct;
                return;
            }
            idx = (idx + 1) & (kG446Slots - 1);
        }
        ++g_g446Failed; // table full: drop (reported, never silent)
    }

    // Resolve rip -> "module!symbol+0x..". Called only from the report, never while suspended.
    void g446Symbolize(uint64_t rip, char *out, size_t outLen)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 511];
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(rip), &disp, sym) != FALSE)
        {
            std::snprintf(out, outLen, "%s+0x%llx", sym->Name,
                          static_cast<unsigned long long>(disp));
            return;
        }
        // No symbol: name the module + RVA so a system DLL (a wait, a GL driver entry) is still
        // identifiable, which is exactly the case the frame's blocked time falls into.
        HMODULE mod = nullptr;
        char modName[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod) &&
            GetModuleFileNameA(mod, modName, MAX_PATH) != 0)
        {
            const char *base = std::strrchr(modName, '\\');
            std::snprintf(out, outLen, "%s+0x%llx", base ? base + 1 : modName,
                          static_cast<unsigned long long>(rip - reinterpret_cast<uint64_t>(mod)));
            return;
        }
        std::snprintf(out, outLen, "0x%llx", static_cast<unsigned long long>(rip));
    }

    // Function start for a rip: SymFromAddr's symbol base, so every PC inside one function folds
    // into one row. Falls back to the module base (system DLLs without exported detail).
    uint64_t g446FuncBase(uint64_t rip)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 511];
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(rip), &disp, sym) != FALSE)
            return static_cast<uint64_t>(sym->Address);
        return rip;
    }

    void g446Report()
    {
        // Fold every sampled PC into its containing FUNCTION before ranking. Ranking raw PCs
        // manufactures a flat distribution out of a concentrated one: a single hot loop spreads
        // its samples over 20-40 distinct rips, so no row clears 1% even when the function owns
        // a quarter of the thread.
        static G446Bucket fn[1024];
        size_t nFn = 0;
        for (size_t i = 0; i < kG446Slots; ++i)
        {
            if (g_g446Table[i].count == 0)
                continue;
            const uint64_t base = g446FuncBase(g_g446Table[i].rip);
            size_t j = 0;
            for (; j < nFn; ++j)
                if (fn[j].rip == base)
                    break;
            if (j == nFn)
            {
                if (nFn == 1024)
                    continue;
                fn[nFn].rip = base;
                fn[nFn].count = 0;
                ++nFn;
            }
            fn[j].count += g_g446Table[i].count;
        }

        static G446Bucket top[40];
        size_t nTop = 0;
        for (size_t i = 0; i < nFn; ++i)
        {
            const G446Bucket &b = fn[i];
            if (nTop < 40)
            {
                top[nTop++] = b;
            }
            else
            {
                size_t worst = 0;
                for (size_t j = 1; j < 40; ++j)
                    if (top[j].count < top[worst].count)
                        worst = j;
                if (top[worst].count < b.count)
                    top[worst] = b;
            }
        }
        std::sort(top, top + nTop,
                  [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });

        std::fprintf(stderr,
                     "[G446:gsprof] samples=%llu distinctPc=%llu dropped=%llu periodUs=%u (top %zu)\n",
                     static_cast<unsigned long long>(g_g446Total),
                     static_cast<unsigned long long>(g_g446Distinct),
                     static_cast<unsigned long long>(g_g446Failed),
                     g446SampleUs(), nTop);
        const double denom = (g_g446Total > 0) ? static_cast<double>(g_g446Total) : 1.0;
        for (int s = 0; s <= 7; ++s)
        {
            if (g_g446StageCount[s] == 0)
                continue;
            std::fprintf(stderr,
                         "[G446:stage]   %-15s %5.2f%% n=%llu ofWhichKernelWait=%5.2f%%\n",
                         g446StageName(s),
                         100.0 * static_cast<double>(g_g446StageCount[s]) / denom,
                         static_cast<unsigned long long>(g_g446StageCount[s]),
                         100.0 * static_cast<double>(g_g446StageWait[s]) /
                             static_cast<double>(g_g446StageCount[s]));
        }
        for (size_t i = 0; i < nTop; ++i)
        {
            char name[560];
            g446Symbolize(top[i].rip, name, sizeof(name));
            std::fprintf(stderr, "[G446:gsprof]   %5.2f%% n=%llu rip=0x%llx %s\n",
                         100.0 * static_cast<double>(top[i].count) / denom,
                         static_cast<unsigned long long>(top[i].count),
                         static_cast<unsigned long long>(top[i].rip), name);
        }
    }

    std::atomic<bool> g_g446SamplerStarted{false};

    void g446SamplerMain(HANDLE target)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);

        const unsigned periodUs = g446SampleUs();
        const auto reportEvery = std::chrono::seconds(g446ReportSec());
        auto nextReport = std::chrono::steady_clock::now() + reportEvery;

        CONTEXT ctx;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(periodUs));
            if (SuspendThread(target) == static_cast<DWORD>(-1))
                break; // target gone
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(target, &ctx);
            ResumeThread(target);
            if (ok)
            {
                ++g_g446Total;
                const uint64_t rip = static_cast<uint64_t>(ctx.Rip);
                g446Record(rip);
                int stage = 7;
                if (g_g446Stage != nullptr)
                {
                    const int s = g_g446Stage->load(std::memory_order_relaxed);
                    stage = (s >= 0 && s <= 6) ? s : 7;
                }
                ++g_g446StageCount[stage];
                if (rip < g_g446ExeLo || rip >= g_g446ExeHi)
                    ++g_g446StageWait[stage];
            }
            else
            {
                ++g_g446Failed;
            }
            if (std::chrono::steady_clock::now() >= nextReport)
            {
                g446Report();
                nextReport = std::chrono::steady_clock::now() + reportEvery;
            }
        }
    }
#endif // _WIN32

    // Called once from the GS worker thread's own entry, so the handle is guaranteed to be that
    // thread and no registry/lookup is needed.
    void g446RegisterGsWorkerThread(std::atomic<int> *stageBreadcrumb)
    {
#ifdef _WIN32
        if (!g446GsProfOn())
            return;
        if (g_g446SamplerStarted.exchange(true))
            return;
        g_g446Stage = stageBreadcrumb;
        // Exe address range, so "sampled inside a kernel/driver wait" can be told from
        // "sampled inside our own code" without symbolising on the sampling path.
        if (HMODULE self = GetModuleHandleA(nullptr))
        {
            const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(self);
            const IMAGE_NT_HEADERS *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
                reinterpret_cast<const uint8_t *>(self) + dos->e_lfanew);
            g_g446ExeLo = reinterpret_cast<uint64_t>(self);
            g_g446ExeHi = g_g446ExeLo + nt->OptionalHeader.SizeOfImage;
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, 0))
        {
            std::fprintf(stderr, "[G446:gsprof] DuplicateHandle failed (%lu) — sampler disabled\n",
                         GetLastError());
            return;
        }
        std::fprintf(stderr, "[G446:gsprof] armed: sampling the GS worker at %u us, report every %u s\n",
                     g446SampleUs(), g446ReportSec());
        std::thread(g446SamplerMain, dup).detach();
#endif
    }
} // namespace
