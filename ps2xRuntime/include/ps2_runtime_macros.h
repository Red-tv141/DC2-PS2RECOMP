#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstdlib> // G371: getenv for the COP1 saturation rollback lever
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"

// G652 P2/P16: helpers used by generated code. Their bodies live in the runtime library so
// generated translation units keep only the tiny hot-path wrappers here.
void ps2EeWaitScopeEnter();
void ps2EeWaitScopeExit();
PS2Runtime::RecompiledFunction ps2TryRegisteredFunctionFast(PS2Runtime *runtime, uint32_t address);
PS2Runtime::RecompiledFunction ps2LookupFunctionFast(PS2Runtime *runtime, uint32_t address);
PS2Runtime::RecompiledFunction ps2TryRegisteredFunctionRuntimeFast(PS2Runtime *runtime, uint32_t address);
PS2Runtime::RecompiledFunction ps2LookupFunctionRuntimeFast(PS2Runtime *runtime, uint32_t address);

// ================================================================================================
// ⭐⭐⭐ G739 — THE DISPATCH FAST PATH, AT THE CALL SITE
// ================================================================================================
//
// ⛔⛔⛔ THE DEFECT THIS EXISTS TO FIX. The four declarations immediately above are G652's
// "helpers used by generated code". A whole-tree grep (runtime + the 7,807-file `recomp/` corpus)
// finds **ZERO callers of three of them**; only `ps2LookupFunctionRuntimeFast` is used, once, by
// `PS2Runtime::dispatchLoop`. The generator never emitted the other two, so
// `DC2_G652_NO_GENERATED_FASTDISPATCH` has always rolled back a mechanism that did not exist.
//
// What the corpus emits instead, at every one of its 44,220 / 46,551 static call sites
// (`code_generator.cpp` JAL/J/JALR arms):
//
//     if (runtime->hasFunction(0x13E3B0u)) {                 // out-of-line call #1
//         auto targetFn = runtime->lookupFunction(0x13E3B0u); // out-of-line call #2
//
// `dc2_game` is compiled WITHOUT `/GL` (NO-GO row 636 forbids enabling it: blind `/GL /LTCG`
// measured +5.02 ms/f on VU1), so MSVC cannot inline either one. Both are genuine cross-library
// calls into `ps2_runtime.lib`.
//
// ⭐ WHAT THEY COST, from the runtime's OWN counting instrument, not a sampler top-N
// (`runtime_host_display.inc`, the `[G651:disp]` block):
//
//     s05:  lookup = 151,190 calls/f   has = 145,351/f   preempt = 134,653/f   mapFallback = 0
//           [G446:eeprof] lookupFunction 5.24% of a 15.1 ms/f EE thread => ~5.2 ns ~= 19 cycles
//           [G646:dispatch] rate = 100.00%   <- the unordered_map behind the table is NEVER read
//
// Every one of ~296,000 calls per frame is answered by ONE array load. The call is the whole cost.
// G733 6e.4 named this "the best-priced EE lever on the table" and it was never built.
//
// WHAT THIS IS. The same single probe, inlined at the call site. At a `jal`/`j` site `addr` is a
// compile-time constant, so the range test and the index arithmetic FOLD: one bool load, one array
// load, one test. It is a PURE ACCELERATOR over the table G646 already owns -- `m_functionTable`
// stays the single authority, and every miss (out of range, misaligned, cleared, unregistered, or
// ANY observer armed) falls through to the unchanged public API. The answer is identical by
// construction, and the fast path can only ever short-circuit a hit the map also holds.
//
// ⭐ EVERY OBSERVER IS HANDLED BY ONE BOOL, NOT BY A SECOND PREDICATE PER SITE. `g_ps2DirectFast`
// is false unless the table is live AND every diagnostic / statistic / trampoline that
// `g652LookupFunctionFast` tests is dormant (see `g739ArmDispatchFast` for the enumerated list --
// it is copied from that function's own predicate, not from memory). Arm any of them and every
// call site takes the slow path verbatim, which is what keeps `DC2_G646_STAT`,
// `DC2_G651_DISP_STAT`, `DC2_G186_SPBAL`, `DC2_TRACE_HANG` and the G649 dispatch diagnostics
// bit-identical to the pre-G739 binary.
//
// Rollback: DC2_G739_NO_INLINE_DISPATCH=1 -- folds into `g_ps2DirectFast`, so BOTH ARMS LIVE IN
//           ONE BINARY WITH IDENTICAL CODE LAYOUT. That is Rule 46 satisfied without a second
//           link, the same form G734 used to gate DC2_G726_KICK_SPIN on the ship binary.
// Oracle:   DC2_G739_ORACLE=1 -> [G739:oracle] compares this probe against the public API on
//           every dispatch and counts disagreements. It forces `g_ps2DirectFast` false so the
//           comparison runs on the authority path; see `ps2DirectCallSlow`.
// Census:   DC2_G739_STAT=1   -> [G739:disp] fast/slow/miss counts per report.

// The dispatch table's address window. ⭐ SINGLE SOURCE OF TRUTH: `runtime_init_and_signals.inc`
// derives its `kG646Lo` / `kG646Hi` / `kG646Slots` from these, so the inline probe and the
// runtime's own `g646Get` can never disagree about the geometry.
inline constexpr uint32_t PS2_DIRECT_LO = 0x00100000u;
inline constexpr uint32_t PS2_DIRECT_HI = 0x00380000u;
inline constexpr uint32_t PS2_DIRECT_SPAN = PS2_DIRECT_HI - PS2_DIRECT_LO;

// The dispatch history ring that crash reports read. ⛔ It lives HERE, not in the runtime TU,
// because the inline probe must push to the SAME ring the out-of-line `pushDispatchPc` uses --
// two rings would mean `formatDispatchHistory()` printed only the dispatches that happened to take
// the slow path. `uint32_t wrapped` rather than `bool` so the struct is a trivially copyable POD
// with no padding surprises.
struct Ps2DispatchRing
{
    uint32_t pcs[64];
    uint32_t next;
    uint32_t wrapped;
};

// ⛔ NO INITIALISER, ON PURPOSE. A namespace-scope `thread_local` with no initialiser is
// zero-initialised with NO dynamic init, so MSVC emits a plain TLS slot access. Give it a
// constructor or a function-local `static` and MSVC inserts a per-ACCESS `_Init_thread_header`
// epoch check instead -- the exact trap G619 measured and G649/G651 had to fix three times in this
// same call chain (`g_g649HangTrace`, `s_g646DispatchOn`, `s_noPreempt`).
extern thread_local Ps2DispatchRing g_ps2DispatchRing;

extern PS2Runtime::RecompiledFunction *g_ps2DirectTable; // nullptr until the first registration
extern bool g_ps2DirectFast;                             // table live AND every observer dormant
extern bool g_ps2DirectTrace;                            // mirrors g_g651DispTrace (default ON)

// ================================================================================================
// ⭐ G739 (4) — PRICING THE TABLE LOAD ITSELF (`DC2_G739_TBLBALLAST=N`, default 0 = absent)
// ================================================================================================
//
// THE QUESTION. Once the CALL is inlined away (above), the residue of a guest dispatch is ONE
// dependent load out of `g_ps2DirectTable`. That table is 655,360 slots x 8 B = **5.2 MB**, and
// `[G651:disp] distinctTargets ~= 5789` says only ~5,789 of those slots are ever touched — ~370 KB
// of scattered cache lines against a 256 KB L2. At 318,000 probes/frame on `fight:rain` the term is
// somewhere between **0.1 ms/f** (every probe an L1 hit) and **3.5 ms/f** (every probe an L3 hit),
// and nothing in this project has ever measured which.
//
// ⭐ THE INSTRUMENT THAT ASKS IT WAS ALREADY BUILT AND NEVER READ. `g651NoteTarget`'s own comment:
// *"`distinct` is a Bloom-ish coverage estimate of how many DIFFERENT dispatch targets a window
// touches — it decides whether the 5.2 MB direct table is being STREAMED (a cache-miss lever) or
// re-hit (not one)."* It answered 5,789 and no phase acted on it.
//
// THE METHOD IS G738 §2.2's, because that is the only one that survived there: price the EVENT by
// ADDING N more of it and reading `dFrame / (N x probes/f)`. Removal cannot answer this (deleting
// the load deletes the dispatch), and a stage timer inside a PGO TU attributes code motion
// (G735 §2).
//
// ⛔ IT MUST NOT BE ELIDABLE — Rule G738-2, where two of three ballasts priced nothing because the
// driver/compiler skipped them, and the tell was a FLAT response to N. So:
//   * the loaded value is XOR-accumulated into a `volatile` sink, which MSVC may not drop;
//   * the index is derived from the dispatch address itself and walks by a large odd stride, so it
//     lands on other LIVE-range slots rather than a single cached one, and the compiler cannot
//     fold the sequence;
//   * the ballast counts ITS OWN loads (Rule G738-3: a price whose denominator is computed from
//     the flag cannot detect that the flag leaked; one that is measured can).
//
// ⚠️ PROBE ONLY, NEVER PROMOTABLE — it can only make the frame slower. Read the response to N=0/4/16
// and quote the SLOPE between the two ballast points, which cancels any fixed control offset.
//
// ⛔⛔ COMPILE-TIME GATED, AND THE FIRST DRAFT OF THIS FILE GOT IT WRONG. A runtime `if (ballast)`
// here is not free: it puts a global load, a compare and a branch on the fast path of ALL 46,551
// generated dispatch sites of a SHIPPING build, to support a probe that ships disarmed. That is
// precisely NO-GO row 639 / Rule 31 — the defect this very phase fixes in §3.4 for the path-watch
// debugger — and G653 priced ~60 lines of it at +0.484 ms/f in ONE hot TU. Keep diagnostics out of
// hot TUs by COMPILE-TIME exclusion, never by a runtime flag; a phase may not exempt its own
// instrument from the law it is enforcing elsewhere.
//
// Build the price binary with `-DPS2X_G739_TBLBALLAST=ON`; the gate binary and the deliverable
// carry zero bytes of it. Both ballast arms then live in that one binary, so layout still cancels.
#if defined(PS2X_G739_TBLBALLAST)
extern uint32_t g_ps2TblBallast; // 0 when the env arm is absent
extern volatile uint64_t g_ps2TblBallastSink;
extern std::atomic<uint64_t> g_ps2TblBallastLoads;
void ps2TblBallastRun(uint32_t address);
#define PS2_G739_TBL_BALLAST(addr)                                                                 \
    do                                                                                             \
    {                                                                                              \
        if (g_ps2TblBallast != 0u)                                                                 \
            ps2TblBallastRun(addr);                                                                \
    } while (0)
#else
#define PS2_G739_TBL_BALLAST(addr) ((void)0)
#endif

// The unchanged public API, behind one call instead of two. Also the oracle's home.
PS2Runtime::RecompiledFunction ps2DirectCallSlow(PS2Runtime *runtime, uint32_t address);
// `runtime->lookupFunction(address)` verbatim, for the two emission arms that call it WITHOUT a
// `hasFunction` guard and use the result unconditionally. ⛔ Those arms may not use
// `ps2DirectCall`: `lookupFunction` never returns nullptr -- on a miss it returns the recovery
// handler and logs -- so collapsing the two contracts would turn a recoverable bad dispatch into
// a null call.
PS2Runtime::RecompiledFunction ps2LookupDirectSlow(PS2Runtime *runtime, uint32_t address);

#if defined(_MSC_VER)
#define PS2_G739_FORCEINLINE __forceinline
#else
#define PS2_G739_FORCEINLINE inline __attribute__((always_inline))
#endif

// ⛔ `__forceinline`, NOT `inline`. Measured on the ship flags (/O2 /Ob2, no /GL): plain `inline`
// left MSVC emitting a real `call ?ps2PushDispatchPcInline` from the probe's fast path -- which
// would have put a cross-function call back on the exact path this phase exists to take one off.
PS2_G739_FORCEINLINE void ps2PushDispatchPcInline(uint32_t pc)
{
    if (!g_ps2DirectTrace)
        return;
    Ps2DispatchRing &h = g_ps2DispatchRing;
    h.pcs[h.next] = pc;
    h.next = (h.next + 1u) & 63u;
    if (h.next == 0u)
        h.wrapped = 1u;
}

// Returns the registered body for `address`, or nullptr if there is none -- exactly the answer
// `runtime->hasFunction(a) ? runtime->lookupFunction(a) : nullptr` produces today, including the
// `pushDispatchPc` side effect that only fires on a hit.
inline PS2Runtime::RecompiledFunction ps2DirectCall(PS2Runtime *runtime, uint32_t address)
{
    PS2_G739_TBL_BALLAST(address); // §4 price probe; compiles to nothing without PS2X_G739_TBLBALLAST
    if (g_ps2DirectFast && (address - PS2_DIRECT_LO) < PS2_DIRECT_SPAN && (address & 3u) == 0u)
    {
        const PS2Runtime::RecompiledFunction fn =
            g_ps2DirectTable[(address - PS2_DIRECT_LO) >> 2];
        if (fn != nullptr)
        {
            ps2PushDispatchPcInline(address);
            return fn;
        }
    }
    return ps2DirectCallSlow(runtime, address);
}

// Same probe, `lookupFunction`'s contract: never nullptr, recovery handler on a miss.
inline PS2Runtime::RecompiledFunction ps2LookupDirect(PS2Runtime *runtime, uint32_t address)
{
    PS2_G739_TBL_BALLAST(address);
    if (g_ps2DirectFast && (address - PS2_DIRECT_LO) < PS2_DIRECT_SPAN && (address & 3u) == 0u)
    {
        const PS2Runtime::RecompiledFunction fn =
            g_ps2DirectTable[(address - PS2_DIRECT_LO) >> 2];
        if (fn != nullptr)
        {
            ps2PushDispatchPcInline(address);
            return fn;
        }
    }
    return ps2LookupDirectSlow(runtime, address);
}

// ================================================================================================
// ⭐ G739 (2) — THE BACK-EDGE PREEMPTION CHECK, AT THE CALL SITE
// ================================================================================================
//
// `code_generator.cpp` emits `if (runtime->shouldPreemptGuestExecution()) return;` on EVERY
// non-call-like back edge of every recompiled guest loop -- 4,144 static sites, **134,653 calls
// per frame** on `s05`, measured by `[G446:eeprof]` at **6.10% of the whole EE thread on
// `dungeon6`** and 2.29% on `s05` (`runtime_dispatch_and_memory.inc`, the G651 note).
//
// The body's answer is `false` on 98-99% of those calls, and reaching that answer costs a
// cross-library call, a TLS materialisation for the counter, a load of two namespace-scope bools,
// an ACQUIRE atomic load of `m_guestExecutionWaiters`, and the ret.
//
// EXACTNESS. The real rule yields when a per-thread counter reaches an interval that is 64 when a
// waiter is queued on the guest-execution mutex and 100 when none is. The inline form reproduces
// the answer exactly as long as it compares against THAT interval -- and the two early-outs above
// it (`DC2_G57_NO_PREEMPT`, the scoped title-draw suppression) only ever return false, so they
// cannot make a sub-interval count yield. The counter is the SAME `thread_local` object in both
// forms and the increment happens in exactly one place, so the two cannot drift.
//
// ⛔⛔ A FIXED THRESHOLD OF 64 IS NOT GOOD ENOUGH, AND THIS PHASE CAUGHT THAT IN ITS OWN FIX BEFORE
// GATING IT. 64 is the MINIMUM interval, so comparing against it is exact -- but with no waiter
// queued the real interval is 100, and every back edge from 64 to 99 would fall through to the
// out-of-line call only to be told "not yet". On `fight:rain` that is 36% of 132,941 calls/frame
// still paying the call this lever exists to remove: the mechanism would have delivered 64% of its
// own prize while reading as perfectly correct. Publishing the live interval fixes it.
//
// ⚠️ THE ONE DIVERGENCE, STATED RATHER THAN WAVED AWAY. The authority re-reads the waiter count on
// every call, so it notices a NEWLY ARRIVED waiter immediately and shortens 100 -> 64 mid-interval.
// The inline form notices at its next slow entry, so a waiter that arrives mid-interval is served
// up to 36 back edges later than before. Back edges run 63,000-133,000 per frame, the 64-vs-100
// pair is a fairness heuristic and not a contract, and `DC2_G739_NO_INLINE_PREEMPT=1` restores the
// old behaviour exactly.
//
// ⛔ `DC2_G651_DISP_STAT` DISARMS THE INLINE FORM. Otherwise `[G651:disp] preempt=` would count one
// call in 100 instead of every call -- an instrument that silently became a 1% sample of its own
// population. Rule 44's class. `DC2_G57_NO_PREEMPT` disarms it too, so that experiment keeps its
// exact "this function is never reached" shape rather than "reached 1 time in 100".
//
// Rollback: DC2_G739_NO_INLINE_PREEMPT=1.
extern thread_local uint32_t g_ps2BackEdgeCounter;
extern bool g_ps2PreemptFast;
// The interval the authority last computed. Relaxed atomic: on x86-64 a relaxed load is one `mov`,
// so it costs what a plain global read costs while staying out of UB when several guest threads
// publish it concurrently. Both values it can ever hold are valid, so a racing write cannot produce
// a wrong decision -- only an interval that is one step stale, which is the divergence above.
extern std::atomic<uint32_t> g_ps2PreemptLimit;
bool ps2ShouldPreemptSlow(PS2Runtime *runtime); // counter already advanced by the caller

inline bool ps2ShouldPreempt(PS2Runtime *runtime)
{
    if (!g_ps2PreemptFast)
        return runtime->shouldPreemptGuestExecution();
    if (++g_ps2BackEdgeCounter < g_ps2PreemptLimit.load(std::memory_order_relaxed))
        return false;
    return ps2ShouldPreemptSlow(runtime);
}

// ================================================================================================
// ⭐ G739 (3) — THE PATH-WATCH DEBUGGER IS NOT ON THE STORE FAST PATH ANY MORE
// ================================================================================================
//
// `ps2TraceGuestWrite` (ps2_runtime.h) is a development path-watch: it tests whether a guest store
// intersects the fixed 512-byte window at `PS2_PATH_WATCH_ADDR = 0x01EFFFA0` and logs it. It was
// called on the NON-SPECIAL (fast) branch of every WRITE8/16/32/64/128 -- **65,547 static sites**,
// on the hottest code in the project -- and gated only at RUNTIME, so a shipping build pays a
// null test plus the two-compare range test on every single guest store, forever, to answer a
// question nobody is asking.
//
// ⛔ That is NO-GO row 639 / Rule 31's own class, and this project has measured the class twice:
// G653 priced ~60 lines of inert code in one hot TU at **+0.484 ms/f**, and G710 measured
// `#if`-excluded probe text with `constexpr` stubs -- every use site folding -- at **+0.362 ms/f**
// in `ps2_gs_rasterizer.cpp`. The standing law is "keep diagnostics out of hot TUs by COMPILE-TIME
// exclusion, never by a runtime flag", so that is what this is.
//
// Re-arm with `cmake -DPS2X_G739_PATH_WATCH=ON`; the feature itself is unchanged.
#if defined(PS2X_G739_PATH_WATCH)
#define PS2_TRACE_GUEST_WRITE(...) ps2TraceGuestWrite(__VA_ARGS__)
#else
#define PS2_TRACE_GUEST_WRITE(...) ((void)0)
#endif

inline const bool g_ps2EeWorkStat = (std::getenv("DC2_G182_EE_STAT") != nullptr);

class Ps2EeWaitScope
{
public:
    Ps2EeWaitScope() : m_active(g_ps2EeWorkStat) { if (m_active) ps2EeWaitScopeEnter(); }
    ~Ps2EeWaitScope() { if (m_active) ps2EeWaitScopeExit(); }
    Ps2EeWaitScope(const Ps2EeWaitScope &) = delete;
    Ps2EeWaitScope &operator=(const Ps2EeWaitScope &) = delete;
private:
    bool m_active;
};

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) _mm_add_ps((__m128)(a), (__m128)(b))
#define PS2_VSUB(a, b) _mm_sub_ps((__m128)(a), (__m128)(b))
#define PS2_VMUL(a, b) _mm_mul_ps((__m128)(a), (__m128)(b))
#define PS2_VDIV(a, b) _mm_div_ps((__m128)(a), (__m128)(b))
#define PS2_VMULQ(a, q) _mm_mul_ps((__m128)(a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            PS2_TRACE_GUEST_WRITE(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            PS2_TRACE_GUEST_WRITE(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            PS2_TRACE_GUEST_WRITE(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            PS2_TRACE_GUEST_WRITE(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            PS2_TRACE_GUEST_WRITE(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
#define PS2_PABSW(a) _mm_abs_epi32((__m128i)(a))
#define PS2_PABSH(a) _mm_abs_epi16((__m128i)(a))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
#define PS2_PSLLVW(a, b) _mm_custom_sllv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRLVW(a, b) _mm_custom_srlv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRAVW(a, b) _mm_custom_srav_epi32((__m128i)(a), (__m128i)(b))

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// ---------------------------------------------------------------------------
// FPU (COP1) operations — R5900 semantics (G371)
//
// The EE FPU is NOT IEEE-754. It has no Infinity, no NaN and no denormals: every
// arithmetic result is saturated to +/-0x7F7FFFFF (1.7014118e38), and a division by zero
// yields that same saturated magnitude with the sign of the operands. Guest code is written
// against that: it keeps computing on saturated-but-FINITE values, and its own `!= 0.0` /
// `<` guards keep working.
//
// Emitting host IEEE arithmetic instead breaks that contract in two steps. First an already
// saturated 1.7014118e38 operand (produced legitimately by a COP2/VU0 macro op, which DOES
// saturate here) overflows to +/-inf on the next multiply or add. Then `inf - inf` (or
// `inf * 0`) produces a NaN, and NaN defeats every comparison the guest uses to protect
// itself — `x != 0.0f` is TRUE for NaN, so the zero-length guards in the camera/matrix code
// are silently bypassed.
//
// G371 measured exactly that chain in the first cutscene: the EE camera matrix in
// mgRENDER_INFO (+0x1a0) and the camera position (+0x3a0) go NaN in X and Z (Y stays finite)
// while the projection stays clean; the poison then rides the VIF into VU1, where G370 had
// already traced it to an all-zero transformed vertex, a saturating 1/w divide, and finally
// the NaN `q` that puts every vertex of the scene at (0,0) -> flat gray screen.
//
// Saturating here is the hardware behaviour, so it is default-ON. Rollback to the previous
// IEEE arithmetic with DC2_G371_NO_FPUCLAMP=1.
//
// Deliberately NOT done here (would be a second mechanism, and the game shows no dependence
// on it): denormal flush-to-zero on operands and results.
// ---------------------------------------------------------------------------

// 0x7F7FFFFF — the largest magnitude an R5900 COP1 register can hold.
#define PS2_FPU_MAXF 3.4028234663852886e+38f

// Namespace-scope inline variable, NOT a function-local static: this is read by every single
// guest float op, and a magic-static guard load per op is exactly the class of per-op cost the
// G268 rule is about. `getenv` runs once, at static-init time, before any guest code executes.
inline const bool g_ps2FpuNoClamp = (std::getenv("DC2_G371_NO_FPUCLAMP") != nullptr);

// G372: denormal flush-to-zero, the other half of the non-IEEE format. The EE has no denormal
// encoding at all — an operand or result below the smallest normal is a signed zero. This is
// exactly what the COP2/VU0-macro FMAC path has always done (its flag block folds `exp == 0 &&
// mantissa != 0` to the sign bit alone); COP1 simply never did. Separate kill switch from the
// saturation half so the two can be bisected independently.
#define PS2_FPU_MIN_NORMALF 1.17549435e-38f
inline const bool g_ps2FpuNoDenormFlush = (std::getenv("DC2_G372_NO_DENORM") != nullptr);

// Saturate an arithmetic result the way the EE FPU does. The NaN case cannot arise once every
// op is clamped, but a NaN can still be LOADED from guest memory written before this was on
// (or by a path that bypasses COP1), so it is folded to the saturated magnitude too.
inline float ps2_fpu_finish(float v)
{
    if (g_ps2FpuNoClamp)
        return v;
    if (v != v)
        return PS2_FPU_MAXF;
    if (v > PS2_FPU_MAXF)
        return PS2_FPU_MAXF;
    if (v < -PS2_FPU_MAXF)
        return -PS2_FPU_MAXF;
    if (!g_ps2FpuNoDenormFlush && v > -PS2_FPU_MIN_NORMALF && v < PS2_FPU_MIN_NORMALF)
        return std::signbit(v) ? -0.0f : 0.0f;
    return v;
}

// DIV.S: a zero denominator sets the D flag and returns the saturated magnitude, signed by the
// XOR of the operand signs (this includes 0/0). No Infinity is ever produced.
inline float ps2_fpu_div(float a, float b)
{
    if (g_ps2FpuNoClamp)
    {
        if (b == 0.0f)
            return copysignf(INFINITY, a * 0.0f);
        return a / b;
    }
    if (b == 0.0f)
        return (std::signbit(a) != std::signbit(b)) ? -PS2_FPU_MAXF : PS2_FPU_MAXF;
    return ps2_fpu_finish(a / b);
}

// SQRT.S: the EE takes the square root of the ABSOLUTE value (a negative operand only raises
// the I flag); host sqrtf would return NaN.
inline float ps2_fpu_sqrt(float a)
{
    if (g_ps2FpuNoClamp)
        return sqrtf(a);
    return sqrtf(fabsf(a));
}

// ---------------------------------------------------------------------------
// COP2 / VU0 macro-mode Q register (G372)
//
// NOTE for whoever reads this next: the VU0-macro **FMAC** ops (VADD/VSUB/VMUL/VMADD/...) are
// already PS2-correct and need nothing here — the MAC-flag block the code generator emits after
// every one of them re-encodes the result (`exp == 0x7F800000` -> sign|0x7F7FFFFF, denormal ->
// signed zero) *after* computing the flags from the raw value, which is both the hardware
// saturation and the hardware flag semantics. Do not "fix" PS2_VADD/PS2_VMUL by clamping inside
// them: that would compute the O/U flags from an already-clamped value and break them.
//
// What was NOT covered is the lower-pipeline Q ops, which bypass the FMAC path entirely and were
// emitted with invented fallbacks rather than hardware behaviour:
//   VDIV   was `(ft != 0) ? fs/ft : 0.0f`   -> hardware returns +/-MAX (sign = XOR of signs)
//   VSQRT  was `sqrtf(max(0, ft))`          -> hardware square-roots the ABSOLUTE value
//   VRSQRT was `(ft > 0) ? 1/sqrtf(ft) : 0` -> hardware computes `fs / sqrt(|ft|)`; it is a
//                                              DIVIDE and it does not ignore fs
// A normalize whose length underflows therefore produced Q = 0 (collapsing the direction to the
// zero vector) where hardware produces a huge-but-finite Q. Same family as G371.
// ---------------------------------------------------------------------------
inline float ps2_vu_divq(float fs, float ft)
{
    if (g_ps2FpuNoClamp)
        return (ft != 0.0f) ? (fs / ft) : 0.0f;
    return ps2_fpu_div(fs, ft);
}

inline float ps2_vu_sqrtq(float ft)
{
    if (g_ps2FpuNoClamp)
        return sqrtf(ft < 0.0f ? 0.0f : ft);
    return ps2_fpu_finish(sqrtf(fabsf(ft)));
}

inline float ps2_vu_rsqrtq(float fs, float ft)
{
    if (g_ps2FpuNoClamp)
        return (ft > 0.0f) ? (1.0f / sqrtf(ft)) : 0.0f;
    return ps2_fpu_div(fs, sqrtf(fabsf(ft)));
}

#define FPU_ADD_S(a, b) ps2_fpu_finish((float)(a) + (float)(b))
#define FPU_SUB_S(a, b) ps2_fpu_finish((float)(a) - (float)(b))
#define FPU_MUL_S(a, b) ps2_fpu_finish((float)(a) * (float)(b))
#define FPU_DIV_S(a, b) ps2_fpu_div((float)(a), (float)(b))
#define FPU_SQRT_S(a) ps2_fpu_sqrt((float)(a))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ((float)(int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
#define FPU_CVT_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_EQ_S(a, b) ((float)(a) == (float)(b))
#define FPU_C_UEQ_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLT_S(a, b) ((float)(a) < (float)(b))
#define FPU_C_ULT_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLE_S(a, b) ((float)(a) <= (float)(b))
#define FPU_C_ULE_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SEQ_S(a, b) ((float)(a) == (float)(b))
#define FPU_C_NGL_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LT_S(a, b) ((float)(a) < (float)(b))
#define FPU_C_NGE_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LE_S(a, b) ((float)(a) <= (float)(b))
#define FPU_C_NGT_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(2, 3, 0, 1)), _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(0, 3, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
