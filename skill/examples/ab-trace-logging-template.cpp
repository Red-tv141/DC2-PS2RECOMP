// ============================================================================
//  A/B Trace Logging Template — "Side B" of the PCSX2 comparison (12 §3)
//
//  Purpose: emit the recompiled runtime's state at a chosen guest function in
//  a format that DIFFS CLEANLY against what you read from PCSX2 at the same
//  point (pcsx2_read_registers / pcsx2_read_memory). First divergence = root
//  cause location.
//
//  Rules baked in:
//   - ONE line per event, fixed field order, grep-able prefix "AB|".
//   - Env-gated, default OFF (lever doctrine, 15 §5). Never ship enabled.
//   - Uncapped hit COUNTER always; full lines only for the first N hits
//     (context survival — a 500KB log is 15% of your context, SKILL §6).
//   - Hook via a game override that falls through to the ORIGINAL function
//     (observe, don't alter — a probe that changes behavior proves nothing).
// ============================================================================

#include "game_overrides.h"
#include "ps2_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>

namespace {

// --- config ------------------------------------------------------------------
constexpr uint32_t kTraceAddr   = 0x001D9410u;  // guest fn to observe (SET ME)
constexpr int      kMaxFullLogs = 32;           // full lines; counter is uncapped

std::atomic<uint64_t> g_hits{0};

inline bool traceEnabled()
{
    static const bool on = std::getenv("MYGAME_AB_TRACE") != nullptr;
    return on;
}

// One line, fixed order. Same order you will read from pcsx2_read_registers:
//   AB|<tag>|hit=<n>|pc=%08X|a0..a3|t0|sp|ra|v0|gp
// For memory probes add:  |m@<addr>=%08X
void emitLine(const char* tag, R5900Context* ctx, uint8_t* rdram, uint32_t memProbe)
{
    const uint64_t n = g_hits.load();
    std::printf("AB|%s|hit=%llu|pc=%08X|a0=%08X|a1=%08X|a2=%08X|a3=%08X"
                "|t0=%08X|sp=%08X|ra=%08X|v0=%08X|gp=%08X",
                tag, static_cast<unsigned long long>(n), ctx->pc,
                getRegU32(ctx, 4), getRegU32(ctx, 5),
                getRegU32(ctx, 6), getRegU32(ctx, 7),
                getRegU32(ctx, 8), getRegU32(ctx, 29),
                getRegU32(ctx, 31), getRegU32(ctx, 2),
                getRegU32(ctx, 28));
    if (memProbe) {
        const uint32_t v =
            *reinterpret_cast<uint32_t*>(rdram + (memProbe & 0x01FFFFFF));
        std::printf("|m@%08X=%08X", memProbe, v);
    }
    std::printf("\n");
}

// --- the probe: log ENTRY state, run the ORIGINAL, log EXIT (v0) --------------
void abProbe(uint8_t* rdram, R5900Context* ctx, PS2Runtime* rt)
{
    const uint64_t n = ++g_hits;                    // uncapped — always counts
    const bool full = traceEnabled() && n <= kMaxFullLogs;

    if (full) emitLine("in ", ctx, rdram, /*memProbe=*/0);

    // Fall through to the real recompiled body so behavior is UNCHANGED.
    // Mechanism depends on your runtime: call the original handler pointer you
    // saved before registering this probe, e.g.:
    //   g_original(rdram, ctx, rt);
    // If the runtime lacks a "call original" path, probe the CALLER instead,
    // or set the log at function EXIT via a breakpoint-style hook.
    (void)rt;

    if (full) emitLine("out", ctx, rdram, /*memProbe=*/0);
}

void applyAbTrace(PS2Runtime& runtime)
{
    // Remember Prohibition #13: this fires only for INDIRECT calls unless the
    // target is a TOML stub. Verify the probe actually hits (counter > 0)
    // before drawing ANY conclusion — "0 hits" means not reached, not "fine".
    runtime.registerFunction(kTraceAddr, abProbe);
}

} // namespace

PS2_REGISTER_GAME_OVERRIDE("ab-trace", "SLUS_XXX.XX", 0u, 0u, &applyAbTrace);

// ============================================================================
//  SIDE A (PCSX2) — capture the matching line:
//    pcsx2_pause(); pcsx2_set_breakpoint("0x001D9410"); pcsx2_continue();
//    pcsx2_read_registers(category=0)   -> a0..a3, t0, sp, ra, v0, gp
//  Transcribe into the SAME field order, then diff line-vs-line:
//    a0..a3/t0 differ at entry  -> divergence is UPSTREAM (in the caller/args)
//    entry matches, v0 differs  -> divergence is INSIDE this function
//    entry+exit match           -> this function is innocent; move the probe
//                                  downstream (bisect the call chain).
//  At session end: kill switch off, breakpoints cleared (12 §5).
// ============================================================================
