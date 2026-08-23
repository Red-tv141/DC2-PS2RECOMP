// ===== G493: LATE DRAIN — give the async render something to overlap with ======================
//
// PREMISE, re-taken on the current binary before a line was written (the G476 law).
//
//   control (shipped, synchronous)
//     [G447:edge] windowMs/f=22.92-24.82 backend-render=3.34-3.79/34.0
//                 rdcol=3.06-3.33/6.0 wrcol=0.38-0.61/11.0 copy=0.09-0.13/2.0 other=2.91-3.24/20.0
//                 | blocked=10.58-11.40 cpu=12.32-13.42
//   DC2_G444_ASYNC=1
//     [G447:edge] windowMs/f=23.73-24.98 backend-render=2.39-2.54/18.0
//                 rdcol=3.14-3.33/6.0 wrcol=0.42-0.45/11.0 copy=0.09-0.12/2.0 other=2.82-3.14/20.0
//                 | blocked=9.41-10.01 cpu=14.25-14.96
//
// The async break already works. 16 of the 34 render round trips lose their blocking wait — exactly
// the 47% that [G305:elig] reports eligible (elig=57818/flush=122880) — every other edge is
// unchanged, and `failed=0` over 59,000 packets. `blocked` falls ~1.1 ms/f. And `cpu` RISES ~1.6
// ms/f, because drainAsync() is not inside a G447 edge scope, so the wait it still performs is
// charged to `cpu`. The frame does not move.
//
// THE WAIT RELOCATES; IT DOES NOT DISAPPEAR. That is why G452 measured this mechanism neutral
// (+0.136 ms/f) and why the render bucket's ceiling has never been collected: nothing was ever
// wrong with the ownership transfer, the packet simply had no CPU to overlap with.
//
// ROOT. g305DrainPending() sits at the TOP of g178TryFlushGpu, before the flush does any of its own
// preparation. The overlap window handed to packet N is therefore only
// [submit(N) .. top of flush N+1] — the optimistic epilogue plus whatever guest work separates two
// flushes. The GS worker's 12.3-13.4 ms/f of CPU is almost all INSIDE g178TryFlushGpu, i.e. AFTER
// that drain, and the async render is never allowed to run against any of it.
//
// THE LEVER. Move that ONE drain from the top of the flush to immediately before the flush's own
// submit — after batch building, materialization, window publication and argument validation.
// Packet N then overlaps the whole of flush N+1's preparation: ~0.42 ms of front CPU per flush
// (14.4 ms/f over 34 flushes) against ~0.14 ms of render work per async item (2.5 ms/f over 18).
//
// WHY IT IS SAFE. Each of these is a property of the code as it already stands, not a new invariant:
//   * GL ORDER IS UNCHANGED. The backend is a single FIFO worker; every later job — synchronous
//     submit, readColor, view build, copy — is dequeued after the pending render. Draining later
//     reorders nothing.
//   * EVERY OTHER DRAIN SITE STAYS. g261Materialize() and the sprite path still drain before they
//     read a target, so no consumer can observe a target the pending render has not finished
//     writing. This lever MOVES one drain and ADDS none.
//   * THE FAILURE REPLAY IS NOT RUN INLINE. g305DrainOne()'s recovery re-enters g_g144FlushSeqFn(),
//     which rebuilds the flush's `static G178Batch s_batch`. At the late-drain point s_batch is
//     LIVE, so replaying there would clobber the batch about to be submitted. Instead the late
//     drain records the failure in g_g493FailPending and leaves the record queued; the next
//     top-of-flush g305DrainPending() runs the proven replay at the point that is already safe
//     today. Either way the packet is waited on exactly once.
//
// Levers:
//   DC2_G419_AB=latedrain     mechanism gate. Put DC2_G444_ASYNC=1 on BOTH arms so the only
//                             variable between them is the drain POINT.
//   DC2_G493_LATE_DRAIN=1     standalone arm. Combine with DC2_G419_AB=async1 for the PROMOTION
//                             gate: async+latedrain against the shipped synchronous default.
//   DC2_G493_NO_LATE_DRAIN=1  rollback; wins over both.
//   DC2_G493_STAT=1           -> [G493:ovl]. The drain's blocking time and the overlap window
//                             actually handed to each packet. This is the ONLY place the drain wait
//                             is visible — [G447:edge] charges it to `cpu`. Read it in both arms.

static bool g493StatOn()
{
    static const bool on = (std::getenv("DC2_G493_STAT") != nullptr);
    return on;
}

// Defined at global scope in g419_ab_instrument.inc (included earlier in this TU).
int g493LateDrainArm();

static bool g493LateDrainActive()
{
    static const bool rollback = (std::getenv("DC2_G493_NO_LATE_DRAIN") != nullptr);
    if (rollback)
        return false;
    const int arm = g493LateDrainArm(); // -1 unless DC2_G419_AB=latedrain
    if (arm >= 0)
        return arm == 1;
    // G640's master arm IMPLIES the late drain: the deep ring is pointless if the packet is retired
    // at the top of the next flush, before that flush has done any of its own preparation.
    // (g640AsyncOn is forward-declared above g305DrainRequired in the including file.)
    static const bool forced = (std::getenv("DC2_G493_LATE_DRAIN") != nullptr);
    return forced || g640AsyncOn();
}

// Raised when the late drain observed a backend failure. The record stays queued and the packet is
// NOT waited on again; the next top-of-flush g305DrainPending() runs the proven replay.
static bool g_g493FailPending = false;

// [G493:ovl] accumulators. Front-end (GS worker) thread only — no atomics needed.
static uint64_t g_g493DrainNs = 0ull, g_g493DrainN = 0ull;
static uint64_t g_g493GapNs = 0ull, g_g493GapN = 0ull;
static std::chrono::steady_clock::time_point g_g493SubmitAt{};

static void g493NoteSubmit()
{
    if (g493StatOn())
        g_g493SubmitAt = std::chrono::steady_clock::now();
}

// Charge the interval [submitAsync returned .. this drain began] to the overlap window, and return
// the drain's start stamp. Called by BOTH drain paths so the two arms are measured identically.
static std::chrono::steady_clock::time_point g493DrainBegin()
{
    if (!g493StatOn())
        return std::chrono::steady_clock::time_point{};
    const auto t0 = std::chrono::steady_clock::now();
    if (g_g493SubmitAt.time_since_epoch().count() != 0)
    {
        g_g493GapNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t0 - g_g493SubmitAt).count());
        ++g_g493GapN;
        g_g493SubmitAt = std::chrono::steady_clock::time_point{};
    }
    return t0;
}

static void g493DrainEnd(const std::chrono::steady_clock::time_point &t0)
{
    if (!g493StatOn() || t0.time_since_epoch().count() == 0)
        return;
    const auto t1 = std::chrono::steady_clock::now();
    g_g493DrainNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++g_g493DrainN;
    if ((g_g493DrainN % 2048ull) == 0ull)
        std::fprintf(stderr,
                     "[G493:ovl] late=%d drains=%llu drainUs=%.1f gapUs=%.1f (n=%llu)\n",
                     g493LateDrainActive() ? 1 : 0,
                     (unsigned long long)g_g493DrainN,
                     static_cast<double>(g_g493DrainNs) / static_cast<double>(g_g493DrainN) / 1000.0,
                     g_g493GapN != 0ull
                         ? static_cast<double>(g_g493GapNs) / static_cast<double>(g_g493GapN) / 1000.0
                         : 0.0,
                     (unsigned long long)g_g493GapN);
}

// Drain the in-flight async packet at the LATE point (immediately before the flush's own submit).
// NEVER runs the failure replay: s_batch is live here (see the header comment). On failure the
// record is left queued and g_g493FailPending is raised, so the next top-of-flush
// g305DrainPending() resolves it without waiting on the packet a second time.
static bool g493LateDrainOne()
{
    if (g_g305Pending.empty() || g_g305ForceSync || g_g493FailPending)
        return false;
    const auto t0 = g493DrainBegin();
    bool ok = true;
    const bool had = g178_backend_drain_async(ok);
    g493DrainEnd(t0);
    if (!had)
        return false; // front-end record with nothing in flight: leave it to the top-of-flush drain
    if (ok)
    {
        g_g305Pending.pop_front();
        g_g305Drained.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }
    g_g493FailPending = true;
    return false;
}

static void g493LateDrainPending()
{
    while (g493LateDrainOne()) {}
}
