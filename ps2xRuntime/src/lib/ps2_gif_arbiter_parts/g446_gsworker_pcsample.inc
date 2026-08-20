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

    // G563: postpone sampling until a route's known heavy window. This keeps a long scripted
    // lead-in from diluting the VU-worker distribution with queue-idle samples. Diagnosis only;
    // the sampler is already opt-in and a zero/missing value preserves its old start time.
    unsigned g563SampleDelaySec()
    {
        static const unsigned s_s = [] {
            const char *v = std::getenv("DC2_G446_GSPROF_DELAY_SEC");
            const unsigned u = (v != nullptr) ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
            return (u <= 600u) ? u : 0u;
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
    // G478: which thread this sampler instance is pointed at (report tag only). The sampler is a
    // singleton — arm at most one of DC2_G446_GSPROF / DC2_G478_VU1PROF per run.
    const char *g_g446Label = "gsprof";
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

    // ===== G503: CALLER ATTRIBUTION FOR OUT-OF-EXE LEAVES =====
    //
    // The instrument's blind spot, found the first time it was pointed at the EE thread. `eeprof`
    // read **35% of that thread inside DLL leaves** — memmove 10.86%, getenv 6.95%, memcmp 6.03%,
    // ZwFreeVirtualMemory 5.69%, NtDelayExecution 5.63% — and a rip-only histogram can name the
    // LEAF but never the CALL SITE, so none of it is actionable. `getenv` in particular is the
    // arc's highest-value pattern (G500: one uncached read on the VU1 path was ~8 ms/f) and the
    // existing name-bucketed census, `[G446:envcensus]`, prints NOTHING because its
    // `g446NoteEnvRead` hook was never wired to the `envFlagEnabled` bodies — a G494-class dead
    // counter, which is why guessing from a grep was the only option before this.
    //
    // Mechanism: when the sampled rip is outside our own image, copy a fixed window of the
    // suspended thread's stack and, after resuming, record the FIRST qword in it that points into
    // our image. That is the return address of the outermost DLL call, i.e. our call site.
    //
    // Exactness caveats, both real:
    //   * It is a HEURISTIC, not an unwind. A stale qword left by an earlier deeper call can win
    //     over the true return address. Read the rows as "our code that leads to this leaf", and
    //     confirm any row that selects a lever by reading the source at that address.
    //   * G497's law still applies on top: a public-symbol name is a fold until the `/MAP` says
    //     otherwise. This adds a second inference on top of the first.
    //
    // The stack read uses ReadProcessMemory rather than a direct dereference so an invalid Rsp
    // returns FALSE instead of faulting inside the sampler thread. Nothing is allocated while the
    // target is suspended (the same rule the rip table already follows).
    constexpr size_t kG446StackWords = 48; // 384 B above Rsp — deep enough for a CRT leaf's frame
    G446Bucket g_g446CallerTable[kG446Slots]{};
    uint64_t g_g446CallerTotal = 0;
    uint64_t g_g446CallerNone = 0;

    // G619: the stack scan accepted any qword inside the IMAGE, and SizeOfImage covers .rdata and
    // .data as well as .text. On the EE thread that let a stale DATA pointer (a vtable, a string
    // literal, a global's address left in the frame) win over the real return address: 63% of the
    // first caller report resolved into one folded pseudo-symbol in an unnamed region of the map,
    // i.e. not code at all. Narrowing the accept window to the CODE section removes that class
    // outright — a return address is always inside .text. Filled at arm time from the PE headers.
    uint64_t g_g619CodeLo = 0, g_g619CodeHi = 0;

    void g446RecordInto(G446Bucket *table, uint64_t rip, uint64_t *distinct)
    {
        uint64_t h = rip * 0x9E3779B97F4A7C15ull;
        size_t idx = static_cast<size_t>(h >> 51) & (kG446Slots - 1);
        for (size_t probe = 0; probe < kG446Slots; ++probe)
        {
            G446Bucket &b = table[idx];
            if (b.rip == rip) { ++b.count; return; }
            if (b.count == 0)
            {
                b.rip = rip;
                b.count = 1;
                if (distinct) ++*distinct;
                return;
            }
            idx = (idx + 1) & (kG446Slots - 1);
        }
        ++g_g446Failed;
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

    bool g563RawPcOnlyOn()
    {
        static const bool s_on = (std::getenv("DC2_G563_RAWPC_ONLY") != nullptr);
        return s_on;
    }

    // G563: DbgHelp folding/line lookup can consume tens of seconds on this 155 MB map/PDB image.
    // A delayed route capture only needs preferred-base PCs, so provide a raw-only report which
    // performs no symbol calls. Same counters and top-64 selection as G483's normal raw section.
    void g563ReportRawPcOnly()
    {
        std::fprintf(stderr,
                     "[G446:%s] samples=%llu distinctPc=%llu dropped=%llu periodUs=%u rawOnly=1\n",
                     g_g446Label,
                     static_cast<unsigned long long>(g_g446Total),
                     static_cast<unsigned long long>(g_g446Distinct),
                     static_cast<unsigned long long>(g_g446Failed), g446SampleUs());
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

        static G446Bucket raw[64];
        size_t nRaw = 0;
        for (size_t i = 0; i < kG446Slots; ++i)
        {
            const G446Bucket &b = g_g446Table[i];
            if (b.count == 0)
                continue;
            if (nRaw < 64)
                raw[nRaw++] = b;
            else
            {
                size_t worst = 0;
                for (size_t j = 1; j < 64; ++j)
                    if (raw[j].count < raw[worst].count)
                        worst = j;
                if (raw[worst].count < b.count)
                    raw[worst] = b;
            }
        }
        std::sort(raw, raw + nRaw,
                  [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });
        for (size_t i = 0; i < nRaw; ++i)
        {
            const uint64_t pref = raw[i].rip >= g_g446ExeLo && raw[i].rip < g_g446ExeHi
                                      ? raw[i].rip - g_g446ExeLo + 0x140000000ull
                                      : 0ull;
            std::fprintf(stderr, "[G483:rawpc]   %5.2f%% n=%llu pref=0x%llx\n",
                         100.0 * static_cast<double>(raw[i].count) / denom,
                         static_cast<unsigned long long>(raw[i].count),
                         static_cast<unsigned long long>(pref));
        }

        // ===== G619: THE OUT-OF-EXE LEAVES, NAMED =====
        //
        // `pref=0x0` above means "this PC is not in our image", which on the EE thread is 37.6% of
        // the distribution and the single largest thing on it. Name those rows by MODULE+RVA (and
        // by the nearest export when one resolves) so the leaf is identifiable at all — but read
        // the export name as a HYPOTHESIS: a DLL without private symbols resolves to the nearest
        // EXPORT BELOW the address, which folds every internal function onto the export in front
        // of it. The RVA is the fact; the name is the guess. Bounded at 24 rows, so this costs 24
        // SymFromAddr calls, not the 8,192-per-row scan that makes g446Report unusable here.
        {
            static G446Bucket ext[24];
            size_t nExt = 0;
            for (size_t i = 0; i < kG446Slots; ++i)
            {
                const G446Bucket &b = g_g446Table[i];
                if (b.count == 0)
                    continue;
                if (b.rip >= g_g446ExeLo && b.rip < g_g446ExeHi)
                    continue; // in our image — already covered by the [G483:rawpc] rows
                if (nExt < 24)
                    ext[nExt++] = b;
                else
                {
                    size_t worst = 0;
                    for (size_t j = 1; j < 24; ++j)
                        if (ext[j].count < ext[worst].count)
                            worst = j;
                    if (ext[worst].count < b.count)
                        ext[worst] = b;
                }
            }
            if (nExt > 0)
            {
                std::sort(ext, ext + nExt,
                          [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });
                for (size_t i = 0; i < nExt; ++i)
                {
                    char name[560];
                    g446Symbolize(ext[i].rip, name, sizeof(name));
                    char modName[MAX_PATH] = {0};
                    uint64_t rva = 0ull;
                    HMODULE mod = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(ext[i].rip), &mod) &&
                        GetModuleFileNameA(mod, modName, MAX_PATH) != 0)
                    {
                        const char *base = std::strrchr(modName, '\\');
                        if (base)
                            std::memmove(modName, base + 1, std::strlen(base + 1) + 1);
                        rva = ext[i].rip - reinterpret_cast<uint64_t>(mod);
                    }
                    std::fprintf(stderr, "[G619:extleaf]   %5.2f%% n=%llu %s+0x%llx  (%s)\n",
                                 100.0 * static_cast<double>(ext[i].count) / denom,
                                 static_cast<unsigned long long>(ext[i].count),
                                 modName[0] ? modName : "?",
                                 static_cast<unsigned long long>(rva), name);
                }
            }
        }

        // ===== G619: OUR CALL SITES, in the raw-only report too =====
        //
        // The symbolised report already prints this block (see g446Report), but on the EE thread
        // that report is unusable: it calls SymFromAddr once per occupied slot FOR EVERY one of the
        // 40 top rows (8,192 x 40 lookups against a 155 MB PDB), and a 120 s route is killed after
        // ~7 rows. The raw-only path exists precisely to skip DbgHelp — but it also skipped the
        // caller table, which is the only thing that can attribute an out-of-exe leaf.
        //
        // On this thread that is not a detail: 37.6% of EE samples land outside our image and the
        // single largest folded row is `getenv` at 20.8%, which a rip-only histogram can never
        // attribute to a call site. Emitting the caller rips in PREFERRED-BASE form lets
        // tools/g504_mapres.py resolve them offline against the linker /MAP, exactly as the
        // [G483:rawpc] rows above are resolved.
        //
        // Same two exactness caveats as the symbolised block (see g446RecordInto): the stack scan
        // is a HEURISTIC, not an unwind, and a public-symbol name is a fold until the /MAP says
        // otherwise. Read a row as "our code that leads to this leaf" and confirm any row that
        // selects a lever by reading the source at that address.
        if (g_g446CallerTotal > 0)
        {
            static G446Bucket craw[32];
            size_t nC = 0;
            for (size_t i = 0; i < kG446Slots; ++i)
            {
                const G446Bucket &b = g_g446CallerTable[i];
                if (b.count == 0)
                    continue;
                if (nC < 32)
                    craw[nC++] = b;
                else
                {
                    size_t worst = 0;
                    for (size_t j = 1; j < 32; ++j)
                        if (craw[j].count < craw[worst].count)
                            worst = j;
                    if (craw[worst].count < b.count)
                        craw[worst] = b;
                }
            }
            std::sort(craw, craw + nC,
                      [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });
            std::fprintf(stderr,
                         "[G619:rawcaller] outOfExeSamples=%llu (%5.2f%% of all) unattributed=%llu "
                         "(stack heuristic)\n",
                         static_cast<unsigned long long>(g_g446CallerTotal),
                         100.0 * static_cast<double>(g_g446CallerTotal) / denom,
                         static_cast<unsigned long long>(g_g446CallerNone));
            for (size_t i = 0; i < nC; ++i)
            {
                const uint64_t pref = (g_g446ExeLo != 0 && craw[i].rip >= g_g446ExeLo &&
                                       craw[i].rip < g_g446ExeHi)
                                          ? craw[i].rip - g_g446ExeLo + 0x140000000ull
                                          : 0ull;
                // 32 SymFromAddr + 32 SymGetLineFromAddr64 total — bounded, unlike g446Report's
                // per-row full-table scan. FILE:LINE is what makes a row actionable, because the
                // public-symbol name of a `static` is a fold (G497/G478) and the caller of a CRT
                // leaf is very often exactly such a static helper.
                char cname[560];
                g446Symbolize(craw[i].rip, cname, sizeof(cname));
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisp = 0;
                const char *file = "?";
                unsigned long lineNo = 0ul;
                if (SymGetLineFromAddr64(GetCurrentProcess(), static_cast<DWORD64>(craw[i].rip),
                                         &lineDisp, &line) != FALSE &&
                    line.FileName != nullptr)
                {
                    const char *base = std::strrchr(line.FileName, '\\');
                    file = base ? base + 1 : line.FileName;
                    lineNo = line.LineNumber;
                }
                std::fprintf(stderr, "[G619:eecaller]   %5.2f%% n=%llu pref=0x%llx %s:%lu  %s\n",
                             100.0 * static_cast<double>(craw[i].count) / denom,
                             static_cast<unsigned long long>(craw[i].count),
                             static_cast<unsigned long long>(pref), file, lineNo, cname);
            }
        }
    }

    void g446Report()
    {
        if (g563RawPcOnlyOn())
        {
            g563ReportRawPcOnly();
            return;
        }
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
                     "[G446:%s] samples=%llu distinctPc=%llu dropped=%llu periodUs=%u (top %zu)\n",
                     g_g446Label,
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
        // G478: a public-symbol name is a hypothesis (G471/G476) — every `static` folds into the
        // preceding exported symbol, so the top rows of this report have repeatedly named the wrong
        // function. If the PDB carries line info, SymGetLineFromAddr64 resolves the FILE:LINE the
        // sample actually landed on, which no fold can corrupt. Printed for the fn-base rip AND
        // for that function's single hottest raw PC, since the base of a folded row belongs to the
        // wrong function while the hot PC belongs to the real one.
        for (size_t i = 0; i < nTop; ++i)
        {
            char name[560];
            g446Symbolize(top[i].rip, name, sizeof(name));
            uint64_t hotPc = top[i].rip;
            uint64_t hotN = 0;
            for (size_t s = 0; s < kG446Slots; ++s)
            {
                if (g_g446Table[s].count == 0)
                    continue;
                if (g446FuncBase(g_g446Table[s].rip) != top[i].rip)
                    continue;
                if (g_g446Table[s].count > hotN)
                {
                    hotN = g_g446Table[s].count;
                    hotPc = g_g446Table[s].rip;
                }
            }
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(), static_cast<DWORD64>(hotPc), &lineDisp,
                                     &line) != FALSE)
            {
                const char *base = line.FileName ? std::strrchr(line.FileName, '\\') : nullptr;
                std::fprintf(stderr, "[G446:%sline]   %5.2f%% hotPc=0x%llx n=%llu %s:%lu\n",
                             g_g446Label,
                             100.0 * static_cast<double>(top[i].count) / denom,
                             static_cast<unsigned long long>(hotPc),
                             static_cast<unsigned long long>(hotN),
                             base ? base + 1 : line.FileName, line.LineNumber);
            }
            std::fprintf(stderr, "[G446:%s]   %5.2f%% n=%llu rip=0x%llx %s\n", g_g446Label,
                         100.0 * static_cast<double>(top[i].count) / denom,
                         static_cast<unsigned long long>(top[i].count),
                         static_cast<unsigned long long>(top[i].rip), name);
        }

        // G503: OUR CALL SITES for every sample that landed outside our image. See the
        // g446RecordInto note above for why (35% of the EE thread was unattributable DLL leaves)
        // and for the two exactness caveats (stack heuristic, then G497's fold law on top).
        if (g_g446CallerTotal > 0)
        {
            static G446Bucket ctop[20];
            size_t nC = 0;
            for (size_t i = 0; i < kG446Slots; ++i)
            {
                const G446Bucket &b = g_g446CallerTable[i];
                if (b.count == 0)
                    continue;
                const uint64_t base = g446FuncBase(b.rip);
                size_t at = kG446Slots;
                for (size_t j = 0; j < nC; ++j)
                    if (ctop[j].rip == base) { at = j; break; }
                if (at != kG446Slots)
                    ctop[at].count += b.count;
                else if (nC < 20)
                {
                    ctop[nC].rip = base;
                    ctop[nC].count = b.count;
                    ++nC;
                }
                else
                {
                    size_t worst = 0;
                    for (size_t j = 1; j < 20; ++j)
                        if (ctop[j].count < ctop[worst].count)
                            worst = j;
                    if (ctop[worst].count < b.count)
                    {
                        ctop[worst].rip = base;
                        ctop[worst].count = b.count;
                    }
                }
            }
            std::sort(ctop, ctop + nC,
                      [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });
            std::fprintf(stderr,
                         "[G446:%scaller] outOfExeSamples=%llu (%5.2f%% of all) unattributed=%llu "
                         "(stack heuristic — confirm any row that selects a lever)\n",
                         g_g446Label,
                         static_cast<unsigned long long>(g_g446CallerTotal),
                         100.0 * static_cast<double>(g_g446CallerTotal) / denom,
                         static_cast<unsigned long long>(g_g446CallerNone));
            for (size_t i = 0; i < nC; ++i)
            {
                char cname[560];
                g446Symbolize(ctop[i].rip, cname, sizeof(cname));
                const uint64_t pref = (g_g446ExeLo != 0 && ctop[i].rip >= g_g446ExeLo &&
                                       ctop[i].rip < g_g446ExeHi)
                                          ? ctop[i].rip - g_g446ExeLo + 0x140000000ull
                                          : 0ull;
                std::fprintf(stderr,
                             "[G446:%scaller]   %5.2f%% n=%llu pref=0x%llx %s\n", g_g446Label,
                             100.0 * static_cast<double>(ctop[i].count) / denom,
                             static_cast<unsigned long long>(ctop[i].count),
                             static_cast<unsigned long long>(pref), cname);
            }
        }

        // G483: RAW-PC report in PREFERRED-BASE form (DC2_G483_RAWPC=1).
        //
        // WHY. The fn-folded rows above answer "which function", and for this thread the answer has
        // been "82.75% VU1Interpreter::run" since G478 — one function of ~76 KB. That is not an
        // attribution, it is a restatement of the problem. What names the cost is WHERE INSIDE it,
        // and the PDB has no line info (G478), so the only resolver is the linker /MAP (G480).
        // A /MAP lists preferred VAs, so convert here rather than making the reader guess the
        // ASLR slide: preferred = rip - imageBase + 0x140000000.
        //
        // The rows are individual sampled PCs, NOT folded, because a 76 KB function's internal
        // distribution is the entire question.
        {
            static const bool s_rawPc = (std::getenv("DC2_G483_RAWPC") != nullptr);
            if (s_rawPc && g_g446ExeLo != 0)
            {
                static G446Bucket raw[64];
                size_t nRaw = 0;
                for (size_t i = 0; i < kG446Slots; ++i)
                {
                    const G446Bucket &b = g_g446Table[i];
                    if (b.count == 0)
                        continue;
                    if (nRaw < 64)
                        raw[nRaw++] = b;
                    else
                    {
                        size_t worst = 0;
                        for (size_t j = 1; j < 64; ++j)
                            if (raw[j].count < raw[worst].count)
                                worst = j;
                        if (raw[worst].count < b.count)
                            raw[worst] = b;
                    }
                }
                std::sort(raw, raw + nRaw,
                          [](const G446Bucket &a, const G446Bucket &b) { return a.count > b.count; });
                for (size_t i = 0; i < nRaw; ++i)
                {
                    const uint64_t pref = raw[i].rip >= g_g446ExeLo && raw[i].rip < g_g446ExeHi
                                              ? raw[i].rip - g_g446ExeLo + 0x140000000ull
                                              : 0ull;
                    std::fprintf(stderr, "[G483:rawpc]   %5.2f%% n=%llu pref=0x%llx\n",
                                 100.0 * static_cast<double>(raw[i].count) / denom,
                                 static_cast<unsigned long long>(raw[i].count),
                                 static_cast<unsigned long long>(pref));
                }
            }
        }
    }

    std::atomic<bool> g_g446SamplerStarted{false};

    void g446SamplerMain(HANDLE target)
    {
        // G497: SYMOPT_LOAD_LINES was missing, so SymGetLineFromAddr64 below ALWAYS failed and the
        // report printed only public-symbol names. Every `static` function folds into the preceding
        // exported symbol, so those names are hypotheses (the G478 note below says so) — and without
        // lines there was no way to test them. G497's first gsprof read
        // `g415_backend_set_color_window 12.51%` on a thread whose span IS the frame; that symbol is
        // a two-line setter. One missing option cost the arc six phases of "the pole thread is flat".
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);

        const unsigned delaySec = g563SampleDelaySec();
        if (delaySec != 0u)
            std::this_thread::sleep_for(std::chrono::seconds(delaySec));

        const unsigned periodUs = g446SampleUs();
        const auto reportEvery = std::chrono::seconds(g446ReportSec());
        auto nextReport = std::chrono::steady_clock::now() + reportEvery;

        CONTEXT ctx;
        uint64_t stackWin[kG446StackWords];
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(periodUs));
            if (SuspendThread(target) == static_cast<DWORD>(-1))
                break; // target gone
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(target, &ctx);
            // G503: grab the stack window BEFORE resuming — after ResumeThread the frame is gone.
            // Only for out-of-exe rips, which is the only case that needs a caller.
            size_t nStack = 0;
            if (ok && (static_cast<uint64_t>(ctx.Rip) < g_g446ExeLo ||
                       static_cast<uint64_t>(ctx.Rip) >= g_g446ExeHi))
            {
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ctx.Rsp)),
                                      stackWin, sizeof(stackWin), &got) &&
                    got >= sizeof(uint64_t))
                {
                    nStack = static_cast<size_t>(got / sizeof(uint64_t));
                }
            }
            ResumeThread(target);
            if (ok)
            {
                ++g_g446Total;
                const uint64_t rip = static_cast<uint64_t>(ctx.Rip);
                g446Record(rip);
                if (rip < g_g446ExeLo || rip >= g_g446ExeHi)
                {
                    ++g_g446CallerTotal;
                    uint64_t caller = 0ull;
                    // G619: accept only CODE-section qwords (see g_g619CodeLo). Falls back to the
                    // whole-image window when the section walk failed, so the old behaviour is
                    // preserved rather than silently disabling the caller table.
                    const uint64_t lo = (g_g619CodeHi != 0ull) ? g_g619CodeLo : g_g446ExeLo;
                    const uint64_t hi = (g_g619CodeHi != 0ull) ? g_g619CodeHi : g_g446ExeHi;
                    for (size_t i = 0; i < nStack; ++i)
                        if (stackWin[i] >= lo && stackWin[i] < hi)
                        {
                            caller = stackWin[i];
                            break;
                        }
                    if (caller != 0ull)
                        g446RecordInto(g_g446CallerTable, caller, nullptr);
                    else
                        ++g_g446CallerNone;
                }
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
    void g446ArmSamplerOnThisThread(std::atomic<int> *stageBreadcrumb, const char *label)
    {
#ifdef _WIN32
        if (g_g446SamplerStarted.exchange(true))
            return;
        g_g446Label = label;
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
            // G619: the executable-code window, for the caller stack scan (see g_g619CodeLo).
            // BaseOfCode/SizeOfCode is the linker's own answer and needs no section walk.
            if (nt->OptionalHeader.SizeOfCode != 0u)
            {
                g_g619CodeLo = g_g446ExeLo + nt->OptionalHeader.BaseOfCode;
                g_g619CodeHi = g_g619CodeLo + nt->OptionalHeader.SizeOfCode;
            }
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, 0))
        {
            std::fprintf(stderr, "[G446:%s] DuplicateHandle failed (%lu) — sampler disabled\n",
                         label, GetLastError());
            return;
        }
        std::fprintf(stderr, "[G446:%s] armed: sampling at %u us, report every %u s\n",
                     label, g446SampleUs(), g446ReportSec());
        std::thread(g446SamplerMain, dup).detach();
#else
        (void)stageBreadcrumb;
        (void)label;
#endif
    }

    // Called once from the GS worker thread's own entry, so the handle is guaranteed to be that
    // thread and no registry/lookup is needed.
    void g446RegisterGsWorkerThread(std::atomic<int> *stageBreadcrumb)
    {
        if (!g446GsProfOn())
            return;
        g446ArmSamplerOnThisThread(stageBreadcrumb, "gsprof");
    }
} // namespace

// ===== G478: the same sampler, pointed at the VU1 (MTVU) worker thread =====
//
// G476 closed the VU1 direction on a derivative probe that read +0.129 ms/f (t = 1.22) for +1.2 ms
// injected. G477 then took ~1.0 ms of CPU off the GS worker, and the G478 premise gate re-read the
// two probes as GS 1.59 -> +1.21 (sens 0.76) and VU1 1.23 -> +0.99 (sens 0.80): the two workers are
// now a SERIALIZED CHAIN of near-equal sensitivity, so the VU1 worker is a live pole again. It has
// never been sampled — [G303:vu1w] busyMs/f is OCCUPANCY (G434/G476), not a distribution. Default
// OFF; arm with DC2_G478_VU1PROF=1 and never together with DC2_G446_GSPROF.
void g478RegisterVu1WorkerThread()
{
#ifdef _WIN32
    static const bool s_on = (std::getenv("DC2_G478_VU1PROF") != nullptr);
    if (!s_on)
        return;
    g446ArmSamplerOnThisThread(nullptr, "vu1prof");
#endif
}

// ===== G503: the same sampler, pointed at the EE (guest main) thread =====
//
// The one thread this arc has never sampled, and after G503's premise gate the leading pole
// candidate: `[G182:ee] cpuMs/f` is a GetThreadTimes delta reading 16.7-19.3 ms of REAL CPU per
// ~20 ms frame (onCPU 84-96%), while `[G146:perf]` reports the four instrumented EE-side graphics
// paths — VIF1, GIFsubmit, GSimage, GSlocal — at 0.0 each. So the EE burns most of the frame inside
// recompiled guest code, and nothing splits that: `[G182:ee]`'s other column, `busyMs/f`, is the
// wall time between successive mgEndFrame entries, i.e. it BRACKETS THE FRAME and can never rank
// anything (G491's law).
//
// Every caveat of the G446/G478 instances applies: singleton (arm at most one of DC2_G446_GSPROF /
// DC2_G478_VU1PROF / DC2_G503_EEPROF per run), absolute frame numbers taken with it armed are
// inadmissible, only the DISTRIBUTION is. And G497's law bites hardest here: a public-symbol name
// is a HYPOTHESIS until the `/MAP` says it is not a fold — though the runner's `sub_*` bodies are
// external functions, not statics, so they fold less readily than the runtime's anonymous
// namespaces do.
void g503RegisterEeThread()
{
#ifdef _WIN32
    static const bool s_on = (std::getenv("DC2_G503_EEPROF") != nullptr);
    if (!s_on)
        return;
    g446ArmSamplerOnThisThread(nullptr, "eeprof");
#endif
}
