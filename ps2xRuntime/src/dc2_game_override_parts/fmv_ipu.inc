// G375: FMV / movie-playback bring-up (IPU + libmpeg path).
//
// DC2's movie layer (CMovie / VideoDec / ViBuf / VoBuf at 0x2990xx-0x299Cxx) and the
// statically linked Sony libmpeg (0x109xxx-0x10Fxxx) guard every DMA-channel and IPU
// register touch with the libkernel critical-section pair DIntr()/EIntr(). The
// recompiler could not translate the COP0 `di`/`ei` bodies and emitted
// TODO_NAMED("DIntr") / TODO_NAMED("EIntr") instead, so the FIRST guest DMA kick on the
// movie path (setD4_CHCR__FUi@0x299C60, RA=0x299C74) aborted the run with
//   "Unimplemented PS2 stub called: DIntr".
//
// Real hardware semantics (libkernel):
//   int DIntr(void);  // clear EIE, return PREVIOUS enable state (1 = were enabled)
//   int EIntr(void);  // set   EIE, return PREVIOUS enable state
// Callers use the return value to decide whether to re-enable on the way out
// (`old = DIntr(); ...; if (old) EIntr();`), so returning the previous state
// faithfully is what matters — the recompiled runtime has no real EE interrupt
// mask to honour, so the flag is bookkeeping only.
static std::atomic<uint32_t> g_g375_ee_intr_enabled{1u};

static void g375_dintr_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    const uint32_t previous = g_g375_ee_intr_enabled.exchange(0u);
    setReturnU32(ctx, previous);
    ctx->pc = getRegU32(ctx, 31);
}

static void g375_eintr_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    const uint32_t previous = g_g375_ee_intr_enabled.exchange(1u);
    setReturnU32(ctx, previous);
    ctx->pc = getRegU32(ctx, 31);
}

// G376: symbolizing unhandled-exception reporter.
// The runner had no crash reporter at all: an access violation killed the process with
// exit code 0xC0000005 and an EMPTY stderr, which is indistinguishable from a clean exit
// in a headless run. Every recompiled guest function is a real C++ symbol
// (`Name__Fv_0xADDR`), so symbolizing the faulting frames names the GUEST function
// directly. Default ON (it only runs when the process is already dying);
// DC2_G376_NO_CRASH_REPORT=1 disables it.
#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static void g376_report_frame(const char *tag, void *addr)
{
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(addr), &mod);
    char modName[MAX_PATH] = {0};
    if (mod)
        GetModuleFileNameA(mod, modName, sizeof(modName) - 1);

    char symBuf[sizeof(SYMBOL_INFO) + 512] = {0};
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    const bool haveSym =
        SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &disp, sym) != FALSE;

    std::fprintf(stderr, "[G376:crash] %s rip=%p rva=0x%llx module=%s sym=%s+0x%llx\n",
                 tag, addr,
                 mod ? (unsigned long long)(reinterpret_cast<ULONG_PTR>(addr) -
                                            reinterpret_cast<ULONG_PTR>(mod))
                     : 0ull,
                 modName[0] ? modName : "?",
                 haveSym ? sym->Name : "?",
                 haveSym ? (unsigned long long)disp : 0ull);
}

static LONG WINAPI g376_crash_filter(EXCEPTION_POINTERS *info)
{
    static std::atomic<bool> s_reported{false};
    bool expected = false;
    if (!s_reported.compare_exchange_strong(expected, true))
        return EXCEPTION_EXECUTE_HANDLER;

    const EXCEPTION_RECORD *rec = info ? info->ExceptionRecord : nullptr;
    std::fprintf(stderr, "\n[G376:crash] UNHANDLED EXCEPTION code=0x%08lx tid=%lu\n",
                 rec ? (unsigned long)rec->ExceptionCode : 0ul,
                 (unsigned long)GetCurrentThreadId());
    if (rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        std::fprintf(stderr, "[G376:crash]   %s at 0x%llx\n",
                     rec->ExceptionInformation[0] ? "WRITE" : "READ",
                     (unsigned long long)rec->ExceptionInformation[1]);
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    if (rec)
        g376_report_frame("fault", rec->ExceptionAddress);

    // Walk back up the stack so the caller chain (usually the recompiled guest
    // function) is visible even when the fault itself is inside a runtime helper.
    void *frames[32] = {0};
    const USHORT captured = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i)
        g376_report_frame("frame", frames[i]);

    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void g376_install_crash_reporter()
{
    if (dc2_env_flag_enabled("DC2_G376_NO_CRASH_REPORT"))
        return;
    static bool s_installed = []() {
        SetUnhandledExceptionFilter(g376_crash_filter);
        return true;
    }();
    (void)s_installed;
}
#else
static void g376_install_crash_reporter() {}
#endif

// G375 census (opt-in DC2_G375_MOVIE_TRACE=1): which links of the movie chain run.
// The video-out ring (VoBuf) is filled by decBs0 (voBufIncCount) and drained ONLY by
// handler_endimage (voBufDecCount), which the guest registers as a DMAC cause-2
// handler; vblankHandler kicks the image DMA. If the drain half never runs the ring
// fills and decBs0 blocks. Probes call the real bodies unchanged.
static bool g375_movie_trace_enabled()
{
    static const bool on = dc2_env_flag_enabled("DC2_G375_MOVIE_TRACE");
    return on;
}

static void g375_movie_count(const char *tag)
{
    static std::mutex m;
    static std::unordered_map<std::string, uint32_t> counts;
    std::lock_guard<std::mutex> lock(m);
    const uint32_t n = ++counts[tag];
    if (n <= 8u || (n % 300u) == 0u)
        std::fprintf(stderr, "[G375:movie] %s n=%u\n", tag, n);
}

#define G375_MOVIE_PROBE(fnName, sym, tag)                                                  \
    static void fnName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)              \
    {                                                                                       \
        g375_movie_count(tag);                                                              \
        sym(rdram, ctx, runtime);                                                           \
    }

G375_MOVIE_PROBE(g375_vblank_probe, vblankHandler__Fi_0x299800, "vblankHandler")
G375_MOVIE_PROBE(g375_endimage_probe, handler_endimage__Fi_0x2998e0, "handler_endimage")
G375_MOVIE_PROBE(g375_voinc_probe, voBufIncCount__FP5VoBuf_0x299a40, "voBufIncCount")
G375_MOVIE_PROBE(g375_vodec_probe, voBufDecCount__FP5VoBuf_0x299b90, "voBufDecCount")
G375_MOVIE_PROBE(g375_startdisplay_probe, startDisplay__Fi_0x29b820, "startDisplay")
G375_MOVIE_PROBE(g375_stepmain_probe, stepMain__Fv_0x299330, "stepMain")
G375_MOVIE_PROBE(g375_decbs0_probe, decBs0__FP8VideoDec_0x29b8a0, "decBs0")

#undef G375_MOVIE_PROBE

static void applyDC2G375FmvOverrides(PS2Runtime &runtime)
{
    g376_install_crash_reporter();

    if (g375_movie_trace_enabled())
    {
        runtime.registerFunction(0x00299800u, g375_vblank_probe);
        runtime.registerFunction(0x002998E0u, g375_endimage_probe);
        runtime.registerFunction(0x00299A40u, g375_voinc_probe);
        runtime.registerFunction(0x00299B90u, g375_vodec_probe);
        runtime.registerFunction(0x0029B820u, g375_startdisplay_probe);
        runtime.registerFunction(0x00299330u, g375_stepmain_probe);
        runtime.registerFunction(0x0029B8A0u, g375_decbs0_probe);
    }

    // Rollback lever: DC2_G375_NO_INTR=1 restores the pre-G375 TODO stubs.
    static const bool disabled = dc2_env_flag_enabled("DC2_G375_NO_INTR");
    if (disabled)
        return;

    runtime.registerFunction(0x00118BE0u, g375_dintr_stub); // DIntr
    runtime.registerFunction(0x00118C28u, g375_eintr_stub); // EIntr
}
