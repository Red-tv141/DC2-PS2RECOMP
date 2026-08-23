// ===== G640: the asynchronous GS/GL backend pipeline ===========================================
//
// PREMISE, measured this phase on `dragon`'s static tail (never before measured on a GS-poled
// route — every earlier async result, G305/G444/G452/G493, was taken on lean MAP-0).
//
//   [G447:edge] control  render=3.35/49.0 rdcol=2.14/6.0 wrcol=0.29/7.0 copy=0.01/1.0
//                        other=4.30/20.0 | blocked=10.80 cpu=17.92  (window 28.72)
//   [G640:ovl]  control  render(gap1=194us hide1=2.30 hide4=3.09) rdcol(gap1=1089us hide1=1.81)
//                        other(gap1=72us hide1=1.20 hide4=2.83) | ceilK1=5.46 ceilK4=8.36 ms/f
//
// So 5.46 ms/f of the pole's 10.08 ms/f of backend wait IS hideable at pipeline depth one, and
// 8.36 at depth four — against `dragon`'s whole remaining GS-only headroom of +6.3. The CPU to
// overlap with exists on this route; G493's "the packet simply had no CPU to overlap with" was a
// property of MAP-0, not of the mechanism.
//
// AND YET the shipped one-slot break still measures null here:
//
//   [G447:edge] DC2_G444_ASYNC=1 + DC2_G493_LATE_DRAIN=1
//               render=1.58/21.0  rdcol=2.46/6.0  wrcol=0.27/7.0  other=4.97/20.0
//               | blocked=9.98 cpu=18.80  (window 28.78)
//
// 28 of the 49 render round trips lose their blocking wait and the render edge falls -1.77 ms/f —
// but `blocked` falls only -0.79 and `cpu` RISES +0.88, so the pole does not move. Two causes, and
// this phase fixes the one that is a defect:
//
// ⭐⭐⭐ CAUSE 1 (A DEFECT, fixed here). THE ASYNC PATH DRAINS THE GRAPH'S ENTRY-STORAGE POOL.
// `g260ExecuteGraph` recycles each batch's `std::vector<G144Entry>` through `g_g260Pool` — its own
// comment at the swap-back says "reclaim the (now empty) storage's capacity". The async submit
// site defeats it exactly:
//
//     G305Pending pending;              // pending.list is FRESH: capacity 0
//     pending.list.swap(g_g144List);    // g_g144List now has capacity 0
//     g_g305Pending.push_back(std::move(pending));
//
// so the executor's `g_g144List.swap(b.entries); g_g260Pool.push_back(...)` files a ZERO-CAPACITY
// vector into the pool, and the batch's real storage rides inside `g_g305Pending` until the drain
// destroys it. Every async submit therefore costs one full entry-buffer free plus a from-scratch
// regrowth on the next batch — on `dragon`, ~28 submits/frame over batches of thousands of
// `G144Entry`. That is the `cpu` rise, and it has been silently taxing every async measurement on
// this port since G305.
//
// THE FIX IS EXACT AND CHANGES NO OUTPUT: take the pending record's buffer FROM `g_g260Pool`
// before the swap, and give it back to `g_g260Pool` at the drain. The bytes moved, the submission
// order, the failure replay and every drain site are untouched — only which allocation the storage
// comes from changes. Rollback `DC2_G640_NO_POOL=1`.
//
// CAUSE 2 (STRUCTURAL, addressed by depth). The GL backend is a SINGLE FIFO worker whose own
// serial busy is 8.7-9.3 ms/f (`[G332:gsw] backendMs/f`) against the pole's 10.1-10.8 ms/f of
// blocked. With one slot, the item made asynchronous is still executing when the front end reaches
// its NEXT synchronous edge, so the wait migrates from `render` to `rdcol`/`other` instead of
// disappearing — visible above as rdcol +0.32 and other +0.67. A deeper ring lets the worker hold
// several independent packets and keeps the front end running through `gap4` (779 us at the render
// edge) rather than `gap1` (194 us). `DC2_G640_SLOTS=<n>` sets the depth; the front end drains back
// to depth-1 immediately before each submit, so a submit can never find the ring full and fall to
// the CPU replay.
//
// EXACTNESS. Nothing here changes what the GL worker executes or in what order: it is a single FIFO
// and every consumer-side drain (`g261Materialize`, the sprite path, the top-of-flush drain)
// is untouched, so no consumer can observe a target whose pending render has not completed. The
// depth change only alters HOW MANY packets may be outstanding between two drains; the failure
// replay is per-record and still runs exactly once per packet.
//
// Flags (all registered in run_g477_perf.ps1's $clear in the same edit):
//   DC2_G640_ASYNC=1     ⭐ master arm: async admission (G444's relaxation) + late drain + pooled
//                        pending storage + the deep ring.
//   DC2_G640_NO_ASYNC=1  🛡️ master rollback; wins over the arm.
//   DC2_G640_SLOTS=<n>   ring depth, default 4, clamped to 1..8. 1 reproduces the historical depth.
//   DC2_G640_NO_POOL=1   🛡️ rollback of the storage recycling alone (isolates CAUSE 1).
//   DC2_G640_STAT=1      📊 [G640:async] submits / drains / ring-full refusals / pool hits.

static bool g640AsyncOn()
{
    static const bool on = (std::getenv("DC2_G640_ASYNC") != nullptr) &&
                           (std::getenv("DC2_G640_NO_ASYNC") == nullptr);
    return on;
}

static bool g640PoolOn()
{
    static const bool on = g640AsyncOn() && (std::getenv("DC2_G640_NO_POOL") == nullptr);
    return on;
}

// The front end keeps `g_g305Pending` in lockstep with the backend's `m_asyncSlots`, so this must
// agree with G178Backend::g640BackendSlots(). Both read the same variable and clamp identically.
static int g640Depth()
{
    static const int n = [] {
        if (!g640AsyncOn())
            return 1;
        const char *v = std::getenv("DC2_G640_SLOTS");
        long long k = (v != nullptr) ? std::atoll(v) : 0LL;
        if (k <= 0LL)
            k = 4LL;
        if (k > 8LL)
            k = 8LL;
        return static_cast<int>(k);
    }();
    return n;
}

static bool g640StatOn()
{
    static const bool on = (std::getenv("DC2_G640_STAT") != nullptr);
    return on;
}

static uint64_t g_g640PoolHit = 0ull, g_g640PoolMiss = 0ull, g_g640RingFull = 0ull;

// Hand the pending record a buffer that already has capacity, so the swap leaves `g_g144List` — and
// through it `g_g260Pool` — with a usable allocation instead of a zero-capacity husk.
static void g640TakePendingStorage(std::vector<G144Entry> &dst)
{
    if (!g640PoolOn())
        return;
    if (g_g260Pool.empty())
    {
        ++g_g640PoolMiss;
        return;
    }
    dst = std::move(g_g260Pool.back());
    g_g260Pool.pop_back();
    dst.clear();
    ++g_g640PoolHit;
}

// Give a drained record's storage back to the executor's pool rather than destroying it.
static void g640ReturnPendingStorage(std::vector<G144Entry> &&v)
{
    if (!g640PoolOn() || v.capacity() == 0u)
        return;
    v.clear();
    g_g260Pool.push_back(std::move(v));
}

static void g640NoteRingFull()
{
    ++g_g640RingFull;
}

static void g640StatReport()
{
    if (!g640StatOn())
        return;
    static uint64_t s_next = 0ull;
    const uint64_t n = g_g305Submitted.load(std::memory_order_relaxed);
    if (n < s_next)
        return;
    s_next = n + 2000ull;
    std::fprintf(stderr,
                 "[G640:async] depth=%d submitted=%llu drained=%llu failed=%llu ringFull=%llu "
                 "poolHit=%llu poolMiss=%llu pending=%zu\n",
                 g640Depth(), (unsigned long long)n,
                 (unsigned long long)g_g305Drained.load(std::memory_order_relaxed),
                 (unsigned long long)g_g305Failed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g640RingFull,
                 (unsigned long long)g_g640PoolHit,
                 (unsigned long long)g_g640PoolMiss,
                 g_g305Pending.size());
}

// Drain the late-drain ring back to depth-1 immediately before a submit. At depth 1 this is the
// historical `g493LateDrainPending()` (drain everything) verbatim.
static void g640LateDrainToDepth()
{
    const size_t keep = static_cast<size_t>(g640Depth() - 1);
    while (g_g305Pending.size() > keep && g493LateDrainOne())
    {
    }
}
