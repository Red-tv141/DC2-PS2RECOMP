// G653 P7 - decompose the dominant `poolreplay` span into WORK / WAIT / BARRIER.
//
// WHY A SEPARATE TRANSLATION UNIT
//   The accumulators and the reporter belong to `GSRowPool::runCoop`, which lives in
//   `rasterizer_row_pool.inc` inside `ps2_gs_rasterizer.cpp` - a TU that carries hot loops.
//   PS2_PROJECT_STATE.md rule 12b is explicit that a TU has a code-layout budget the way a
//   function has an inline budget: G651 measured +0.567 / +0.934 ms/f of GS own purely from
//   adding ~150 lines of COLD start-up code to the hot GIF TU, and recovered it by moving the
//   identical code elsewhere. So only the atomic adds live in the hot TU; every byte of
//   formatting, ranking and printing lives here.
//
// WHAT THE NUMBERS MEAN
//   G652 P8's census showed `poolreplay` is 69.8% of the `g144` flush, and the G653 baseline
//   shows GS own is the pole on 18 of 21 measured scenes. But "the pool dispatch is 7 ms" does
//   not say whether those 7 ms are rasterization (deletable by a faster kernel) or scheduling
//   (deletable only by a different dispatch). PS2_PROJECT_STATE.md rule 9 - delete work, not
//   waiting - makes that distinction the difference between a real lever and a null one.
//
//     setup    mutex-guarded publish of the partition + notify_all, paid by the caller
//     wake     dispatch -> first WORKER chunk start: how long the pool takes to actually run
//     caller   the caller's own claim loop (its real rasterization plus claim CAS overhead)
//     worker   summed job time across every participant minus the caller's share
//     barrier  caller time spent parked at the join after its own claims ran out
//     sum/wall the aggregate parallel efficiency: sum(work) / (wall * participants)
//
//   A `poolreplay` that is barrier-heavy is TAIL-bound (one unlucky chunk) and is repaired by
//   partitioning, not by a faster inner loop. A `poolreplay` that is nearly all `caller+worker`
//   is genuinely WORK-bound, and only then does vectorizing the span kernels convert.
//
// ARM: DC2_G653_POOLCOOP=1. Never valid inside a timing arm - it puts two steady_clock reads on
// every claimed chunk. Report prints on the same cadence as the other pool censuses.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern const bool g653PoolCoopOn = [] {
    const char *v = std::getenv("DC2_G653_POOLCOOP");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}();

std::atomic<uint64_t> g_g653CoopCalls{0ull};
std::atomic<uint64_t> g_g653CoopSerialCalls{0ull};
std::atomic<uint64_t> g_g653CoopWallNs{0ull};
std::atomic<uint64_t> g_g653CoopSetupNs{0ull};
std::atomic<uint64_t> g_g653CoopCallerNs{0ull};
std::atomic<uint64_t> g_g653CoopChunkNs{0ull};
std::atomic<uint64_t> g_g653CoopBarrierNs{0ull};
std::atomic<uint64_t> g_g653CoopWakeNs{0ull};
std::atomic<uint64_t> g_g653CoopWakeSamples{0ull};
std::atomic<uint64_t> g_g653CoopChunks{0ull};
std::atomic<uint64_t> g_g653CoopCallerChunks{0ull};
std::atomic<uint64_t> g_g653CoopSeats{0ull};
std::atomic<uint64_t> g_g653CoopMaxChunkNs{0ull};

void g653PoolCoopReport()
{
    const uint64_t calls = g_g653CoopCalls.load(std::memory_order_relaxed);
    if (calls == 0ull)
        return;

    const double wall = g_g653CoopWallNs.load(std::memory_order_relaxed) / 1e6;
    const double setup = g_g653CoopSetupNs.load(std::memory_order_relaxed) / 1e6;
    const double caller = g_g653CoopCallerNs.load(std::memory_order_relaxed) / 1e6;
    const double chunk = g_g653CoopChunkNs.load(std::memory_order_relaxed) / 1e6;
    const double barrier = g_g653CoopBarrierNs.load(std::memory_order_relaxed) / 1e6;
    const uint64_t wakeN = g_g653CoopWakeSamples.load(std::memory_order_relaxed);
    const double wake = wakeN ? (g_g653CoopWakeNs.load(std::memory_order_relaxed) / 1e3 / (double)wakeN) : 0.0;
    const uint64_t chunks = g_g653CoopChunks.load(std::memory_order_relaxed);
    const uint64_t cchunks = g_g653CoopCallerChunks.load(std::memory_order_relaxed);
    const uint64_t seats = g_g653CoopSeats.load(std::memory_order_relaxed);
    const double maxChunk = g_g653CoopMaxChunkNs.load(std::memory_order_relaxed) / 1e3;

    // `caller` is measured on the caller's timeline and is therefore already inside `wall`;
    // `chunk` sums every participant's job time, so worker work is the remainder. Both are
    // cumulative - de-cumulate a window against the nearest [G146:frame] n= exactly like the
    // other pool censuses.
    const double worker = (chunk > caller) ? (chunk - caller) : 0.0;
    const double participants = calls ? (1.0 + (double)seats / (double)calls) : 1.0;
    const double efficiency = (wall > 0.0 && participants > 0.0) ? (chunk / (wall * participants)) : 0.0;
    const double barrierShare = (wall > 0.0) ? (100.0 * barrier / wall) : 0.0;
    const double setupShare = (wall > 0.0) ? (100.0 * setup / wall) : 0.0;

    std::fprintf(stderr,
                 "[G653:coop] calls=%llu serial=%llu chunks/call=%.1f callerChunks/call=%.2f "
                 "seats/call=%.2f cumWallMs=%.1f\n",
                 (unsigned long long)calls,
                 (unsigned long long)g_g653CoopSerialCalls.load(std::memory_order_relaxed),
                 calls ? (double)chunks / (double)calls : 0.0,
                 calls ? (double)cchunks / (double)calls : 0.0,
                 calls ? (double)seats / (double)calls : 0.0,
                 wall);
    std::fprintf(stderr,
                 "[G653:coop]   setupMs=%.1f (%.1f%%) callerWorkMs=%.1f workerWorkMs=%.1f "
                 "barrierMs=%.1f (%.1f%%) wakeUs/dispatch=%.2f maxChunkUs=%.1f\n",
                 setup, setupShare, caller, worker, barrier, barrierShare, wake, maxChunk);
    std::fprintf(stderr,
                 "[G653:coop]   sum/wall efficiency=%.3f over %.2f participants  =>  %s\n",
                 efficiency, participants,
                 barrierShare >= 35.0
                     ? "TAIL/SCHEDULING-BOUND: repartition or reduce wake latency before touching the kernels"
                     : (efficiency >= 0.75
                            ? "WORK-BOUND: a faster span kernel converts nearly 1:1"
                            : "MIXED: real work dominates but the join still leaks; re-measure after any kernel win"));
}
