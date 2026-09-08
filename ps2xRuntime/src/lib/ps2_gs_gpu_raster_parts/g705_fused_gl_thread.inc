// ===== G705: the GL backend runs ON the GS front-end thread ====================================
//
// THE DEFECT THIS ADDRESSES. Every GPU operation in this runtime is a THREAD ROUND TRIP. The GS
// front end prepares a batch, pushes a QueueItem, wakes a second thread (`G178Backend::worker`)
// that owns the only GL context, and then blocks until that thread finishes. `[G523:fit]` prices
// the `submit` pass at 0.0688 ms per flush x 50 flushes/f = 3.44 ms/f, and `[G654:submit]` splits
// it `enqueue=0.0767` of `sum=0.0772` -- 99.4% of the pass is the pole thread parked on the GL
// worker. `[G415:stage]` prices the GL worker's OWN renderBatch execution at 2.34 ms/f over 39.9
// batches, so a large part of the gap is handoff, not work.
//
// The handoff is not cheap, and the tree already knows it: G447 promoted a 400 us caller-side
// spin AND a 400 us worker-side spin at -1.5 ms/f, precisely because the kernel wake on this edge
// is slow. G471 then measured ~2.4 ms/f of QueryPerformanceCounter inside those two spin bodies
// and could not remove it -- the clock read is acting as a backoff. So the runtime currently pays,
// per frame: two spin budgets per round trip, a condition-variable wake in each direction, and a
// whole thread's worth of a 6-core budget that G671 already measured at 45.7 CPU-ms/f.
//
// WHY ASYNC SUBMIT COULD NEVER FIX IT. G684/G690/G701/G704 refuted fire-and-forget submit four
// times, and G704 closed it on MECHANISM: "the blocking wait is what YIELDS THE CORE to the GL
// worker; on 6 physical cores running EE + VU1 + GS front + GL worker + an 11-lane row pool +
// audio, removing a wait creates contention, not parallelism."
//
// That refutation is an argument about a TWO-THREAD topology. It says nothing about a ONE-thread
// topology, which is the opposite move: instead of removing the wait and keeping the thread,
// remove the THREAD and the wait goes with it. There is then no core to yield, no wake to hide,
// no spin to budget -- and the asynchrony the G701 arc was chasing is supplied for free by the GL
// driver's own command buffer, which is where PCSX2 gets it.
//
// WHAT THIS DOES. `startEager()` in fused mode creates the shared HGLRC on the main thread but
// makes it current NOWHERE. The first thread to reach `g178_backend_ready()` -- which has exactly
// one call site, the top of `g178TryFlushGpu` -- adopts the context, builds every program and
// buffer on itself, and becomes the owner. From then on every blocking backend call runs its own
// QueueItem INLINE, through the identical `runQueueItem` body the worker thread used.
//
// ORDERING IS UNCHANGED BY CONSTRUCTION. The pump drains `m_queue` in FIFO order before the
// caller's own item can complete, so an enqueue-only producer (G541/G545) still executes ahead of
// the consumer that follows it, exactly as the worker's single-threaded loop guaranteed.
//
// THE FOREIGN-THREAD CASE IS A DECLINE, NOT A HANG. If a thread that does not own the context
// reaches a blocking submit, it cannot execute GL and there is no worker to do it for it. Rather
// than deadlock, the pump removes THAT CALLER'S OWN item from the queue (identified by its
// `doneFlag`, which is unique per `SyncWait`) and answers `false` -- the same answer the backend
// already gives whenever it declines a batch, and one every caller handles by taking the exact CPU
// path. `[G705:fuse]` reports the count so a non-zero population is visible rather than silent.
//
// ⭐⭐⭐ PROMOTED default-ON (G705). `dragon` A/B/B/A, same binary, env arms, `-CooldownSeconds 300`,
// common window scriptFrame 2000..12,607:
//
//   block 1 -0.562 ms/f (-2.29%)   block 2 -0.640 ms/f (-2.62%)
//   POOLED  24.514 -> 23.913 = -0.601 ms/f (-2.45%)   GS own 22.162 -> 21.616 = -0.546
//   control drift 0.132 (need <= 0.30)   route arrival 0.960 (need >= 0.75)   BOTH CHECKS PASS
//   kicks/f 2147.5-2148.3 and pres 4800 on all four arms -- the workload is invariant
//   [G705:fuse] armed=1 foreign=0 on both candidate arms, armed=0 on both control arms
//
// Rule 14 pixel gate on the `ridepod` opening: candidate p90 2.22 / max 20.14 against a
// control-vs-control floor of p90 2.41 / max 36.95 -- BELOW the floor on both statistics, over 58
// shared script keys. Expected: fused GL runs the identical `runQueueItem` body in the identical
// FIFO order, so only the thread that executes it moves.
//
// Rollback: DC2_G705_NO_FUSEGL=1 (restores the worker thread and the per-batch round trip).
bool g705FuseGlOn()
{
    static const bool on = !g158EnvFlag("DC2_G705_NO_FUSEGL") &&
                           !g158EnvDisabled("DC2_G705_FUSEGL");
    return on;
}

bool g705StatOn()
{
    static const bool on = g158EnvFlag("DC2_G705_STAT");
    return on;
}

// Installed by G178Backend once fused mode is armed; null in every other configuration, so the
// shipped `SyncWait::wait()` keeps its exact call shape (17a law 17: the lever-OFF path IS the
// control arm).
//
//   returns true  - not fused, or this thread owns the context and has drained the queue
//   returns false - fused, this thread cannot run GL; the caller's item was cancelled and the
//                   caller must report a declined submit
using G705PumpFn = bool (*)(std::atomic<uint32_t> *doneFlag);
G705PumpFn g_g705Pump = nullptr;

std::atomic<uint64_t> g_g705Pumps{0u};   // inline drains performed by the owner
std::atomic<uint64_t> g_g705Items{0u};   // QueueItems executed inline
std::atomic<uint64_t> g_g705Foreign{0u}; // blocking submits declined because the caller is not the owner
