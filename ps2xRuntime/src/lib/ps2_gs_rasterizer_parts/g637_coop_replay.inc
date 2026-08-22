// ===== G637: the CPU band replay's SECOND-GENERATION EXECUTION ENGINE =====
// revision: 1
//
// THE MEASUREMENT THAT LED HERE (`ridepod`, shipped G636 binary, DC2_G529_STAT=1, 10,471 rendered
// frames, 21,600 dispatches):
//
//   [G529:disp] n=21600 wall=74560.8 lane0=6863.4 worker=255811.1 rt=67697.4
//               bands(l0=21600 wk=151200)   (cumulative ms)
//
// Per rendered frame that is `wall 7.12 / lane0 0.655 / rt 6.47 ms/f`, and per BAND it is
// **0.318 ms for the caller's own band against a 1.692 ms mean worker band — 5.3x**, with a
// 3.45 ms dispatch wall. So:
//
//   * total lane CPU per frame        = 0.655 + 24.43 = **25.09 ms/f** across 8 lanes
//   * wall on the GS thread           = **7.12 ms/f**
//   * parallel efficiency             = 25.09 / 8 / 7.12 = **44%**
//   * a perfectly balanced dispatch   = 25.09 / 8 = **3.14 ms/f**  (headroom ~4.0 ms/f)
//
// and `ridepod` is that headroom's owner: it is the SOLE GS pole (GS own 21.16 vs VU1 17.13 vs EE
// cpu 13.92) with **injection sensitivity 1.05**, and `gsStallMs/f = 0.00`, so the pool wait is
// booked to `GS own` in full and converts ~1:1 to frame time.
//
// ⭐⭐⭐ THE STRUCTURAL DEFECT IS THE PARTITION, NOT THE LANE COUNT.
// `GSRowPool::run(y0, y1, lanes, job)` cuts [y0,y1] into `lanes` EQUAL ROW bands. Equal rows are
// not equal work — a band over the character/boss carries the whole scene and a band over sky
// carries nothing — so the barrier join waits for ONE unlucky band while the other seven idle, and
// the caller (the GS worker thread, this route's pole) blocks in `m_cvDone.wait` after finishing
// the cheapest band of the eight. G529 already refuted the LANE-COUNT lever in the direction it was
// built (`DC2_G529_INLINEREPLAY`, serialising costs 3.9x): the count was never the defect.
//
// ⛔ AND THE STANDING LAW SAYS WAITING IS NOT ENOUGH ON ITS OWN.
// `Delete Work, Not Waiting` (G493/G622): removing a synchronization wait on a SECONDARY thread
// recovers nothing. This wait is not on a secondary thread — it is on the pole itself, inside
// `GS own` — but the aggregate 25.09 ms/f of lane CPU still lands on a 6-core host that is also
// running EE (14.5 ms/f) and VU1 (17.1 ms/f), so this phase deletes work as well as wait:
//
//   1. **Weight-balanced fences** (§1) — cut the row range at equal ESTIMATED work instead of equal
//      rows, so the worst chunk shrinks toward the mean.
//   2. **Over-decomposition + work stealing + GS-thread participation** (`GSRowPool::runCoop`) —
//      more chunks than lanes, one shared lock-free cursor, and the caller claims chunks until the
//      batch is exhausted instead of sleeping. Residual estimator error is absorbed dynamically.
//   3. **The compiled chunk->entry index** (§2) — the fixed eight-lane replay makes every lane walk
//      the WHOLE entry list and bbox-test all of it (8 x 1,456 = 11,648 tests per `discov`
//      dispatch); over-decomposing without this would multiply that by `chunks/lanes`. The index
//      is built ONCE per dispatch and costs exactly the number of (entry, chunk) intersections —
//      the work the walks were doing anyway — so it DELETES aggregate CPU at any chunk count.
//
// ⭐ EXACTNESS IS BY CONSTRUCTION, NOT BY ARGUMENT — see `GSRowPool::runCoop`'s header. Chunks are
// contiguous, disjoint, and cover [y0,y1] exactly once; each replays the whole ordered entry list
// clipped to its own rows. That is the identical property the shipped eight-band replay relies on.
// The chunk->entry index is exact for the same reason: its membership test is literally the
// `e.maxY < a || e.minY > b` predicate the lane loop applies inline, evaluated once instead of
// `chunks` times.
//
//   DC2_G637_COOP=1        the lever (bring-up default: OFF)
//   DC2_G637_NO_COOP=1     rollback once promoted
//   DC2_G637_INDEX=1       ⛔ the compiled chunk->entry index, MEASURED NULL (see g637IndexOn)
//   DC2_G637_BALANCE=1     ⛔ the REFUTED weight-balanced fences (see g637BalanceOn), default OFF
//   DC2_G637_OVER=<n>      chunks per lane (default 4)
//   DC2_G637_MINCHUNK=<n>  minimum rows per chunk (default 2)
//   DC2_G637_ROWBIAS=<n>   per-entry-row constant added to the width weight (weight arm only)
//   DC2_G637_BAL=1         census: wall / sum / max / chunks / caller share per dispatch
//   DC2_G637_VERIFY=1      partition oracle: asserts the fences cover [y0,y1] exactly once

namespace
{

// ⭐⭐⭐ PROMOTED DEFAULT-ON (G638). The order-balanced A B B A payoff gate G637 left outstanding was
// run on `ridepod` (280 s arms, common window scriptFrame 4104..25760, 9,840 presents matched per
// arm) and BOTH order blocks agree in sign:
//     block 1 (A1 vs B1): frame -1.456 ms/f (-6.14%)   GS own -1.392
//     block 2 (A2 vs B2): frame -1.352 ms/f (-5.75%)   GS own -1.272
//     POOLED: 23.613 -> 22.209 = -1.404 ms/f (-5.95%)  GS own 21.076 -> 19.744
// ⛔ It could NOT be gated on `dragon` (the newer, larger GS pole): the band replay is only
// 1.611 ms/f of that route's 33.8 ms/f pole, so its A B B A reads NULL with the blocks disagreeing
// in sign (-0.197 / +0.026). Gate a lever where its mechanism dominates. See phase-G638-fix-log.md.
// Rollback: DC2_G637_NO_COOP=1 — g637RunReplay then forwards to the very same
// `GSRowPool::instance().run(y0, y1, g529Lanes, job)` call, verbatim.
static bool g637CoopOn()
{
    static const bool on = !envFlagEnabled("DC2_G637_NO_COOP");
    return on;
}
// ⛔ MEASURED NULL AT THE SHIPPED OPERATING POINT — DEFAULT OFF, RETAINED AS AN ARM.
// Order-balanced A B B A on `ridepod` (A = index, B = the shipped full walk), `[G637:bal]`
// wall ms/dispatch: A 0.988 / B 0.946 / B 0.932 / A 0.899. **The two order blocks disagree in sign**
// (B−A = −0.042 then +0.033) under a monotonic warm-up drift across the four runs, and the same is
// true of `sum` (−0.232 then +0.044). Mean separation 0.5%, i.e. unmeasurable.
//
// ⭐ WHY THE CEILING WAS SMALLER THAN IT LOOKED: the walk it replaces is `e.maxY < a || e.minY > b`
// over a CONTIGUOUS 373 KB array — two well-predicted compares per entry with hardware prefetch —
// while the index substitutes a gather through an index array AND adds a serial counting sort to
// the dispatcher's prologue (`part` 0.0206 vs 0.0006 ms/dispatch), i.e. it moves work ONTO the pole
// to take it off eight parallel lanes. Default OFF keeps the shipped replay body byte-identical and
// leaves the promoted mechanism a single one: the scheduler.
static bool g637IndexOn()
{
    static const bool on = envFlagEnabled("DC2_G637_INDEX");
    return on;
}
// ⛔ REFUTED BY ITS OWN SWEEP, RETAINED AS THE PROBE THAT REFUTED IT — DEFAULT OFF.
// The weight-balanced partition was this phase's slice 1 and it is WORSE than plain equal rows at
// the same chunk count (`[G637:bal]`, `ridepod`, 31 chunks):
//
//   fences        wall/dispatch   aggregate sum   max chunk   eff
//   weight        1.011 ms        7.119 ms        0.351       7.04
//   equal rows    0.943 ms        6.239 ms        0.547       6.61
//
// ⭐ WHY: over-decomposition plus dynamic claiming ALREADY balances the dispatch, so the fences only
// have to be fine — they do not have to be smart. And a weight-balanced fence set is actively
// harmful, because it makes chunks SHORT exactly where the geometry is dense, so more entries
// straddle a fence and pay their per-draw front-end in more chunks. The scheduler subsumes the
// static estimator and the static estimator costs aggregate CPU to run.
static bool g637BalanceOn()
{
    static const bool on = envFlagEnabled("DC2_G637_BALANCE");
    return on;
}
static bool g637BalStatOn()
{
    static const bool on = envFlagEnabled("DC2_G637_BAL");
    return on;
}
static bool g637VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G637_VERIFY");
    return on;
}

static uint64_t g637EnvU64(const char *name, uint64_t dflt)
{
    const char *e = std::getenv(name);
    if (!e || !*e)
        return dflt;
    const long long v = std::atoll(e);
    return (v <= 0) ? dflt : static_cast<uint64_t>(v);
}
static int g637Over()
{
    static const int v = static_cast<int>(g637EnvU64("DC2_G637_OVER", 4ull));
    return v;
}
static int g637MinChunkRows()
{
    static const int v = static_cast<int>(g637EnvU64("DC2_G637_MINCHUNK", 2ull));
    return v;
}
// The width weight alone models only the PIXELS. Every entry that touches a chunk also pays that
// chunk's share of the per-draw front-end (state load, bbox/scissor clamp, edge setup, admission
// classification) whether it covers one row there or fifty, so a row that many entries merely
// CLIP through is more expensive than its width suggests. This constant is that allowance, in
// units of pixels-of-width, and it is env-tunable so the operating point can be swept without a
// rebuild (g507_probe_at_two_magnitudes).
static int g637RowBias()
{
    static const int v = static_cast<int>(g637EnvU64("DC2_G637_ROWBIAS", 96ull));
    return v;
}

// ---------------------------------------------------------------------------------------------
// §1 The partition. `t_g637Cum[r]` is the CUMULATIVE weight through row r, accumulated with a
// difference array so the pass is O(entries + rows) rather than O(entries * rows) — a per-row
// scatter over 1,456 entries x 252 rows would itself cost more than the imbalance it repairs
// (g526_a_census_that_moves_what_it_measures).
static thread_local std::vector<int64_t> t_g637Cum;
static thread_local std::vector<int> t_g637Bounds;
static thread_local std::vector<int> t_g637RowChunk;
static thread_local std::vector<uint32_t> t_g637IdxOff;  // chunks+1 offsets into t_g637IdxEnt
static thread_local std::vector<uint32_t> t_g637IdxEnt;  // entry indices, ordered per chunk
static thread_local std::vector<uint32_t> t_g637Cur;     // counting-sort fill cursor

// Chunk count. Over-decompose so a mis-estimated chunk can be absorbed by the other participants,
// but keep chunks coarse enough that the per-chunk fixed cost (the state prologue each entry pays
// once per chunk it touches) does not eat the balance win.
static int g637ChunkCount(int rows, int lanes)
{
    if (lanes <= 1 || rows <= 1)
        return 1;
    int c = lanes * g637Over();
    const int minRows = (g637MinChunkRows() < 1) ? 1 : g637MinChunkRows();
    const int byRows = rows / minRows;
    if (c > byRows)
        c = byRows;
    if (c < lanes)
        c = lanes;
    if (c > rows)
        c = rows;
    if (c < 1)
        c = 1;
    return c;
}

// Fills t_g637Bounds with `chunks+1` strictly increasing fences over [y0, y1+1]. Returns the chunk
// count actually produced (never more than `rows`, never less than 1).
static int g637Partition(const std::vector<G144Entry> &list, int y0, int y1, int chunks)
{
    const int rows = y1 - y0 + 1;
    if (chunks < 1)
        chunks = 1;
    if (chunks > rows)
        chunks = rows;
    t_g637Bounds.clear();
    if (chunks <= 1)
    {
        t_g637Bounds.push_back(y0);
        t_g637Bounds.push_back(y1 + 1);
        return 1;
    }

    int64_t total = 0;
    const bool balance = g637BalanceOn();
    if (balance)
    {
        t_g637Cum.assign(static_cast<size_t>(rows) + 1u, 0);
        const int64_t bias = static_cast<int64_t>(g637RowBias());
        for (const auto &e : list)
        {
            // The SAME row interval the lane loop's `e.maxY < a || e.minY > b` prefilter uses,
            // clipped to the dispatch range. Nothing here may disagree with §2's membership test.
            int r0 = e.minY, r1 = e.maxY;
            if (r0 < y0) r0 = y0;
            if (r1 > y1) r1 = y1;
            if (r1 < r0)
                continue;
            const int64_t w =
                static_cast<int64_t>(e.ctx.scissor.x1 >= e.ctx.scissor.x0
                                         ? (static_cast<int>(e.ctx.scissor.x1) -
                                            static_cast<int>(e.ctx.scissor.x0) + 1)
                                         : 0) +
                bias;
            t_g637Cum[static_cast<size_t>(r0 - y0)] += w;
            t_g637Cum[static_cast<size_t>(r1 - y0 + 1)] -= w;
        }
        int64_t run = 0, cum = 0;
        for (int i = 0; i < rows; ++i)
        {
            run += t_g637Cum[static_cast<size_t>(i)];
            cum += run;
            t_g637Cum[static_cast<size_t>(i)] = cum;
        }
        total = cum;
    }

    t_g637Bounds.push_back(y0);
    if (!balance || total <= 0)
    {
        // Equal rows — the shipped G143/G168 partition, kept as the isolation arm and as the
        // degenerate fallback when no entry carries weight.
        for (int c = 1; c < chunks; ++c)
            t_g637Bounds.push_back(y0 + static_cast<int>(static_cast<int64_t>(rows) * c / chunks));
    }
    else
    {
        int prev = 0;
        for (int c = 1; c < chunks; ++c)
        {
            const int64_t target = total * c / chunks;
            int f = prev + 1;
            const int hi = rows - (chunks - c); // leave >= 1 row for each remaining chunk
            while (f < hi && t_g637Cum[static_cast<size_t>(f - 1)] < target)
                ++f;
            if (f > hi)
                f = hi;
            if (f <= prev)
                f = prev + 1;
            t_g637Bounds.push_back(y0 + f);
            prev = f;
        }
    }
    t_g637Bounds.push_back(y1 + 1);

    if (g637VerifyOn())
    {
        static std::atomic<uint64_t> s_bad{0}, s_tot{0};
        bool ok = (t_g637Bounds.size() == static_cast<size_t>(chunks) + 1u) &&
                  t_g637Bounds.front() == y0 && t_g637Bounds.back() == y1 + 1;
        for (int c = 0; ok && c < chunks; ++c)
            ok = t_g637Bounds[static_cast<size_t>(c) + 1u] > t_g637Bounds[static_cast<size_t>(c)];
        s_tot.fetch_add(1u, std::memory_order_relaxed);
        if (!ok)
        {
            const uint64_t bad = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (bad <= 16u)
                std::fprintf(stderr,
                             "[G637:partbad] n=%llu y0=%d y1=%d chunks=%d fences=%zu\n",
                             (unsigned long long)bad, y0, y1, chunks, t_g637Bounds.size());
        }
        if ((s_tot.load(std::memory_order_relaxed) % 4000ull) == 0ull)
            std::fprintf(stderr, "[G637:partvfy] tot=%llu bad=%llu\n",
                         (unsigned long long)s_tot.load(std::memory_order_relaxed),
                         (unsigned long long)s_bad.load(std::memory_order_relaxed));
    }
    return chunks;
}

// ---------------------------------------------------------------------------------------------
// §2 The chunk -> entry index, built once per dispatch on the dispatcher thread.
//
// The shipped lane body is `for (const auto &e : g_g144List) { if (e.maxY < a || e.minY > b)
// continue; ... }` — every lane walks all 1,456 entries of a `discov` batch. That is 11,648 bbox
// tests per dispatch at 8 lanes and would be 46,592 at 32 chunks. A counting-sort bucket over the
// SAME predicate costs one pass over the entries plus exactly one slot per (entry, chunk)
// intersection, i.e. it replaces the walks with the work the walks were looking for.
//
// ⭐ Order is preserved because pass 2 visits entries in submission order, so each bucket comes out
// ascending in entry index — which is what the lane loop's in-order replay requires.
static void g637BuildIndex(const std::vector<G144Entry> &list, int y0, int y1, int chunks)
{
    const int rows = y1 - y0 + 1;
    t_g637RowChunk.assign(static_cast<size_t>(rows), 0);
    for (int c = 0; c < chunks; ++c)
        for (int r = t_g637Bounds[static_cast<size_t>(c)];
             r < t_g637Bounds[static_cast<size_t>(c) + 1u]; ++r)
            t_g637RowChunk[static_cast<size_t>(r - y0)] = c;

    t_g637IdxOff.assign(static_cast<size_t>(chunks) + 1u, 0u);
    const int *rowChunk = t_g637RowChunk.data();
    uint32_t *off = t_g637IdxOff.data();
    const size_t n = list.size();
    for (size_t i = 0; i < n; ++i)
    {
        const G144Entry &e = list[i];
        int r0 = e.minY, r1 = e.maxY;
        if (r0 < y0) r0 = y0;
        if (r1 > y1) r1 = y1;
        if (r1 < r0)
            continue;
        const int c0 = rowChunk[r0 - y0];
        const int c1 = rowChunk[r1 - y0];
        for (int c = c0; c <= c1; ++c)
            ++off[c + 1];
    }
    for (int c = 0; c < chunks; ++c)
        t_g637IdxOff[static_cast<size_t>(c) + 1u] += t_g637IdxOff[static_cast<size_t>(c)];

    t_g637IdxEnt.resize(t_g637IdxOff[static_cast<size_t>(chunks)]);
    // Fill cursor: a copy of the offsets, consumed as the buckets fill. Everything the inner loop
    // touches is taken as a RAW POINTER first — a `thread_local` container indexed inside the loop
    // is an MSVC per-access TLS guard (`_Init_global_epoch`), and this loop runs once per
    // (entry, chunk) intersection on the dispatcher, i.e. on the serial prologue of the pole.
    t_g637Cur.assign(t_g637IdxOff.begin(), t_g637IdxOff.end() - 1);
    uint32_t *cur = t_g637Cur.data();
    uint32_t *ent = t_g637IdxEnt.data();
    for (size_t i = 0; i < n; ++i)
    {
        const G144Entry &e = list[i];
        int r0 = e.minY, r1 = e.maxY;
        if (r0 < y0) r0 = y0;
        if (r1 > y1) r1 = y1;
        if (r1 < r0)
            continue;
        const int c0 = rowChunk[r0 - y0];
        const int c1 = rowChunk[r1 - y0];
        for (int c = c0; c <= c1; ++c)
            ent[cur[c]++] = static_cast<uint32_t>(i);
    }
}

// The band body reads these through the two accessors below. They are written before the dispatch
// is armed and read-only for its duration, so no synchronisation beyond the pool's own arm/join.
static const uint32_t *g_g637IdxOff = nullptr;
static const uint32_t *g_g637IdxEnt = nullptr;
static int g_g637IdxY0 = 0;
static const int *g_g637IdxBounds = nullptr;
static int g_g637IdxChunks = 0;

// Maps a claimed row range back to its chunk index. The pool hands the job [a,b]; the fences are
// strictly increasing, so `a` identifies the chunk uniquely.
static int g637ChunkOf(int a)
{
    if (!g_g637IdxBounds || g_g637IdxChunks <= 0)
        return -1;
    // Chunk counts are small (<= lanes * OVER) and the caller/worker claim order is roughly
    // ascending, so a linear scan beats a branchy binary search here.
    for (int c = 0; c < g_g637IdxChunks; ++c)
        if (g_g637IdxBounds[c] == a)
            return c;
    return -1;
}

// ---------------------------------------------------------------------------------------------
// §3 `[G637:bal]` — the census that prices the scheduler, and the ONLY one that can say whether a
// dispatch wall is imbalance or dispatch overhead. `sum` is the aggregate lane CPU, `max` the worst
// single chunk. `wall ~ max` means one chunk owns the tail (imbalance/preemption); `wall >> max`
// means the wake/join machinery does (overhead). Two clock reads per CHUNK (~66/frame), not per
// pixel and not per entry, so it is not a census that moves what it measures.
static std::atomic<uint64_t> g_g637CurSum{0};
static std::atomic<uint64_t> g_g637CurMax{0};

struct G637Bal
{
    uint64_t n = 0;
    uint64_t wallNs = 0;
    uint64_t sumNs = 0;
    uint64_t maxNs = 0;
    uint64_t chunks = 0;
    uint64_t callerChunks = 0;
    uint64_t partNs = 0; // partition + index build, serial on the dispatcher
};
static thread_local G637Bal t_g637Bal;

static void g637NoteDispatch(uint64_t wallNs, uint64_t partNs, int chunks, uint32_t callerChunks)
{
    if (!g637BalStatOn())
        return;
    ++t_g637Bal.n;
    t_g637Bal.wallNs += wallNs;
    t_g637Bal.partNs += partNs;
    t_g637Bal.sumNs += g_g637CurSum.load(std::memory_order_relaxed);
    t_g637Bal.maxNs += g_g637CurMax.load(std::memory_order_relaxed);
    t_g637Bal.chunks += static_cast<uint64_t>(chunks);
    t_g637Bal.callerChunks += callerChunks;
    if ((t_g637Bal.n % 400ull) != 0ull)
        return;
    const double n = static_cast<double>(t_g637Bal.n);
    std::fprintf(stderr,
                 "[G637:bal] n=%llu wall=%.1f sum=%.1f max=%.1f part=%.2f (cumMs) | per-dispatch "
                 "wall=%.3fms sum=%.3fms max=%.3fms chunks=%.1f caller=%.2f eff=%.2f tail=%.2f\n",
                 (unsigned long long)t_g637Bal.n,
                 static_cast<double>(t_g637Bal.wallNs) / 1e6,
                 static_cast<double>(t_g637Bal.sumNs) / 1e6,
                 static_cast<double>(t_g637Bal.maxNs) / 1e6,
                 static_cast<double>(t_g637Bal.partNs) / 1e6,
                 static_cast<double>(t_g637Bal.wallNs) / 1e6 / n,
                 static_cast<double>(t_g637Bal.sumNs) / 1e6 / n,
                 static_cast<double>(t_g637Bal.maxNs) / 1e6 / n,
                 static_cast<double>(t_g637Bal.chunks) / n,
                 static_cast<double>(t_g637Bal.callerChunks) / n,
                 t_g637Bal.wallNs ? static_cast<double>(t_g637Bal.sumNs) /
                                        static_cast<double>(t_g637Bal.wallNs)
                                  : 0.0,
                 t_g637Bal.wallNs ? static_cast<double>(t_g637Bal.maxNs) /
                                        static_cast<double>(t_g637Bal.wallNs)
                                  : 0.0);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// §4 The dispatch entry point. Byte-identical to the shipped call when the lever is off: it forwards
// to the very same `GSRowPool::run(y0, y1, lanes, job)`.
static void g637RunReplay(int y0, int y1, int lanes,
                          const std::vector<G144Entry> &list,
                          const std::function<void(int, int)> &job)
{
    if (!g637CoopOn() || y1 < y0)
    {
        GSRowPool::instance().run(y0, y1, lanes, job);
        return;
    }
    const int cap = GSRowPool::instance().laneCap();
    if (lanes > cap)
        lanes = cap;
    if (lanes < 1)
        lanes = 1;
    const int rows = y1 - y0 + 1;
    if (lanes <= 1)
    {
        // The pool's own serial path, unchanged.
        GSRowPool::instance().run(y0, y1, lanes, job);
        return;
    }

    const bool bal = g637BalStatOn();
    const auto t0 = bal ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};

    const int want = g637ChunkCount(rows, lanes);
    const int chunks = g637Partition(list, y0, y1, want);
    const bool useIndex = g637IndexOn() && chunks > 0;
    if (useIndex)
        g637BuildIndex(list, y0, y1, chunks);
    g_g637IdxOff = useIndex ? t_g637IdxOff.data() : nullptr;
    g_g637IdxEnt = useIndex ? t_g637IdxEnt.data() : nullptr;
    g_g637IdxBounds = t_g637Bounds.data();
    g_g637IdxChunks = chunks;
    g_g637IdxY0 = y0;

    const auto t1 = bal ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{};

    uint32_t callerChunks = 0;
    if (bal)
    {
        g_g637CurSum.store(0u, std::memory_order_relaxed);
        g_g637CurMax.store(0u, std::memory_order_relaxed);
        GSRowPool::instance().runCoop(
            t_g637Bounds.data(), chunks, lanes - 1,
            [&job](int a, int b) {
                const auto c0 = std::chrono::steady_clock::now();
                job(a, b);
                const uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - c0)
                        .count());
                g_g637CurSum.fetch_add(ns, std::memory_order_relaxed);
                uint64_t cur = g_g637CurMax.load(std::memory_order_relaxed);
                while (ns > cur &&
                       !g_g637CurMax.compare_exchange_weak(cur, ns, std::memory_order_relaxed))
                {
                }
            },
            &callerChunks);
    }
    else
    {
        GSRowPool::instance().runCoop(t_g637Bounds.data(), chunks, lanes - 1, job, &callerChunks);
    }

    g_g637IdxOff = nullptr;
    g_g637IdxEnt = nullptr;
    g_g637IdxBounds = nullptr;
    g_g637IdxChunks = 0;

    if (bal)
    {
        const auto t2 = std::chrono::steady_clock::now();
        g637NoteDispatch(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()),
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
            chunks, callerChunks);
    }
}

} // namespace
