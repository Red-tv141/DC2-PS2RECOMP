// ===== G589: THE DRAIN TRANSACTION — GPU-VU COMMAND/AUTHORITY ARCHITECTURE, SLICE 1 =====
//
// THE MEASUREMENT THAT DEFINES THIS PHASE (S05 forest cutscene, the hardest GS-resource window,
// `n=1021..2461`, post-G588 Release binary, GPU P0/1620 MHz):
//
//   [G260:stat]  closes 56.82/frame   drains 10.26/frame   =>  5.5 BATCHES PER DRAIN
//   [G523:merge] mergeable 0.00%      blocked 65.8%        nopair 34.2%
//   [G523:fit]   per-flush FIXED cost 0.1875 ms x 56.8     =>  10.65 ms/frame
//   [G299:backend] queue latency 5-7 us          backend work 51-72 us
//   [G415:stage] ~40% of backend work is surface UPLOAD, ~17% readback, only ~18% drawing
//
// Read together these say three things, and the architecture follows from them:
//
//  1. THE FLUSH COUNT CANNOT BE REDUCED BY MERGING. One GPU flush is exactly one closed G260Batch
//     and a batch closes only on a target switch, so the flush count IS the target-switch count.
//     G523's merge-headroom census measured ZERO batches that could legally coalesce with an
//     earlier same-(fbp,fbw) batch: 65.8% have such a partner but an intervening batch genuinely
//     depends on them, 34.2% have no partner at all. Coalescing is REFUTED, not deferred.
//
//  2. TWO THIRDS OF THE PER-FLUSH FRONT-END IS FIXED. The least-squares fit of each pass against
//     its flush's entry count puts 66% of the 0.284 ms/flush in the INTERCEPT — cost a flush pays
//     carrying zero entries. `deps` is 87% fixed, `tex` 78%, `submit` 70% (91% against DRAWS).
//     Fixed cost is recoverable only by paying it fewer times.
//
//  3. THE DRAIN, NOT THE BATCH, IS THE NATURAL UNIT. 56.8 closes against 10.26 drains means the
//     whole-flush preparation runs 5.5 times per drain. A drain is atomic with respect to the
//     guest: it is one uninterrupted loop on the GS-worker thread, and nothing outside the batches
//     themselves can mutate guest VRAM, residency or ownership while it runs.
//
// So: DO NOT merge batches (refuted), DO NOT overlap the backend (G493 settled that the GS front
// thread and the GL worker do not compose on this host — the mechanism works and the wall clock
// comes back as contention). REDUCE THE WORK by paying flush-invariant preparation once per
// TRANSACTION instead of once per batch.
//
// WHAT THIS SLICE DOES. It introduces the transaction scope itself plus the first memoized leaf:
// `g310EnsureCurrent()`, the logical/T8 atlas refresh that `[G323]` ranks as the largest single
// dependency leaf (55% of `deps`, ~1.88 ms/frame on S05). It is re-run for every batch even though
// its inputs are RESIDENCY METADATA, not pixels: `g310BuildDesired` reads `g_g261Res[]` ownership
// fields, `g_g280Ovl[]` overlay depth and the 128-page legacy winner map; `g310ApplyDesired` then
// reconciles against the producer page generations.
//
// EXACTNESS ARGUMENT. The memo is not keyed on "nothing happened" — it is SELF-VALIDATING. Before
// reusing a result it recomputes a complete key over every input the memoized function reads:
//   - per resident target: dirty / dirtyLo / dirtyHi / covLo / covHi / fbw / fbW / range /
//     mirPending — exactly the fields g310BuildDesired's layout guard and page-winner walk read;
//   - per resident target: the overlay tile count g_g280Ovl[t].size(), which that same guard reads;
//   - per resident target: g_g310ProducerN[t], the producer-write counter bumped by every
//     g310NoteProducerWrite, i.e. the input g310ApplyDesired reconciles against;
//   - the atlas's own g_g310Initialized / g_g310Frozen / g_g310RefreshPending / g_g310FrozenInvalid
//     lifecycle flags.
// If any of those moved, the key differs and the real function runs. This is why the memo does not
// have to enumerate mutation sites: IT OBSERVES THE INPUTS, NOT THE WRITERS. The key costs ~170
// scalar mixes against a call the census prices at tens of microseconds.
//
// The memo is scoped to ONE transaction and dropped at its end, so it can never carry state across
// a guest-visible boundary, and it is disarmed for the rest of the transaction whenever a batch
// leaves the pure-GPU path (a CPU band replay writes guest VRAM and the private depth mirror
// through paths that publish no residency metadata).
//
// FLAGS
//   DC2_G589_DRAIN=1     arm the memo (DEFAULT-OFF in this slice)
//   DC2_G589_NO_DRAIN=1  rollback once promoted
//   DC2_G589_CENSUS=1    [G589:drain] behaviour-pure: batches/drain, and the AMORTIZATION
//                        HEADROOM — the share of non-first batches in a transaction whose key is
//                        unchanged from the previous batch. This is the number that prices every
//                        further leaf, so it is measured before anything else is built.
//   DC2_G589_VERIFY=1    same-run oracle: on every memo HIT run the real function anyway and
//                        compare its result and its key. Implies the memo. NEVER valid for timing
//                        (it does strictly more work than either arm).
//   DC2_G589_STAT=1      [G589:memo] hit/miss counters only.
//   DC2_G419_AB=drainmemo  paired within-process arm.

// `g589DrainMemoArm` is DEFINED with external linkage in g419_ab_instrument.inc and DECLARED at
// global scope in ps2_gs_rasterizer.cpp. It must not be re-declared here: this file is inside an
// anonymous namespace, and a declaration here would name a different, never-defined symbol (the
// G568 lesson).

static bool g589CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G589_CENSUS");
    return on;
}

static bool g589VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G589_VERIFY");
    return on;
}

static bool g589StatOn()
{
    static const bool on = envFlagEnabled("DC2_G589_STAT");
    return on;
}

static bool g589Paired()
{
    static const bool paired = []() {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "drainmemo") == 0;
    }();
    return paired;
}

// ENV-LEVEL selection, deliberately ARM-INDEPENDENT. Everything that must be symmetric between the
// two paired buckets — the transaction scope, the batch counter, the disarm hook — keys off this,
// so the control arm executes the identical scaffolding and only the memo itself differs
// (G520 §7b: arm symmetry is a codegen property, not an intention).
static bool g589MemoSelected()
{
    static const bool on =
        g589Paired() ||
        ((envFlagEnabled("DC2_G589_DRAIN") || envFlagEnabled("DC2_G589_VERIFY")) &&
         !envFlagEnabled("DC2_G589_NO_DRAIN"));
    return on;
}

// Per-call, honours the paired arm. Never cached: the arm flips every A/B tick.
static bool g589DrainMemoOn()
{
    if (!g589MemoSelected())
        return false;
    if (g589Paired())
        return g589DrainMemoArm() > 0;
    return true;
}

// Any instrument or arm that needs the transaction scope to exist at all.
static bool g589Armed()
{
    static const bool on =
        g589CensusOn() || g589StatOn() || g589VerifyOn() || g589MemoSelected();
    return on;
}

// The transaction is nested-safe by construction: g260ExecutePrefix is called from
// g271ExecuteConflictPrefix, and a CPU band replay can never re-enter the scheduler
// (t_g144InReplay guards both entry points), so depth never exceeds one. The counter is kept
// anyway so a future re-entrant path fails CLOSED (memo disarmed) rather than silently sharing
// state across two logical drains.
static int g_g589Depth = 0;
static uint32_t g_g589BatchIndex = 0u; // batch position inside the open transaction

// Memoized g310EnsureCurrent result for the open transaction.
static bool g_g589AtlasValid = false;   // a result is memoized for this transaction
static bool g_g589AtlasResult = false;  // the memoized return value
static uint64_t g_g589AtlasKey = 0u;    // the key that result was computed under

// Census: amortization headroom. `prevKey` is the previous batch's key inside this transaction.
static uint64_t g_g589PrevKey = 0u;
static bool g_g589PrevKeyValid = false;

static uint64_t g_g589Drains = 0u, g_g589Batches = 0u, g_g589Later = 0u,
                g_g589LaterSameKey = 0u, g_g589MaxBatches = 0u;
static uint64_t g_g589Hits = 0u, g_g589Misses = 0u, g_g589Disarms = 0u;
static uint64_t g_g589VerifyChecks = 0u, g_g589VerifyBadResult = 0u, g_g589VerifyBadKey = 0u;

static inline void g589Mix(uint64_t &h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

// The complete input key of g310EnsureCurrent. Nothing here reads guest VRAM: the memoized
// function does not either.
static uint64_t g589AtlasKeyNow()
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        g589Mix(h, static_cast<uint64_t>(r.dirty ? 1u : 0u) |
                       (static_cast<uint64_t>(r.mirPending ? 1u : 0u) << 1) |
                       (static_cast<uint64_t>(r.range.valid ? 1u : 0u) << 2));
        g589Mix(h, static_cast<uint64_t>(static_cast<uint32_t>(r.dirtyLo)) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(r.dirtyHi)) << 32));
        g589Mix(h, static_cast<uint64_t>(static_cast<uint32_t>(r.covLo)) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(r.covHi)) << 32));
        g589Mix(h, static_cast<uint64_t>(r.fbw) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(r.fbW)) << 32));
        g589Mix(h, static_cast<uint64_t>(r.range.first) |
                       (static_cast<uint64_t>(r.range.last) << 32));
        g589Mix(h, static_cast<uint64_t>(g_g280Ovl[t].size()));
        g589Mix(h, g_g310ProducerN[t].load(std::memory_order_relaxed));
    }
    g589Mix(h, static_cast<uint64_t>(g_g310Initialized ? 1u : 0u) |
                   (static_cast<uint64_t>(g_g310Frozen ? 1u : 0u) << 1) |
                   (static_cast<uint64_t>(g_g310RefreshPending ? 1u : 0u) << 2) |
                   (static_cast<uint64_t>(g_g310FrozenInvalid ? 1u : 0u) << 3));
    return h;
}

static void g589Report()
{
    if (g589CensusOn() && g_g589Drains != 0u && (g_g589Drains % 2000u) == 0u)
    {
        const double later = static_cast<double>(std::max<uint64_t>(1u, g_g589Later));
        std::fprintf(stderr,
                     "[G589:drain] drains=%llu batches=%llu batches/drain=%.2f maxBatches=%llu "
                     "later=%llu sameKey=%llu (%.1f%% HEADROOM) disarms=%llu\n",
                     static_cast<unsigned long long>(g_g589Drains),
                     static_cast<unsigned long long>(g_g589Batches),
                     static_cast<double>(g_g589Batches) /
                         static_cast<double>(std::max<uint64_t>(1u, g_g589Drains)),
                     static_cast<unsigned long long>(g_g589MaxBatches),
                     static_cast<unsigned long long>(g_g589Later),
                     static_cast<unsigned long long>(g_g589LaterSameKey),
                     100.0 * static_cast<double>(g_g589LaterSameKey) / later,
                     static_cast<unsigned long long>(g_g589Disarms));
    }
    const uint64_t calls = g_g589Hits + g_g589Misses;
    if ((g589StatOn() || g589VerifyOn()) && calls != 0u && (calls % 4000u) == 0u)
        std::fprintf(stderr,
                     "[G589:memo] calls=%llu hits=%llu (%.1f%%) misses=%llu "
                     "verify(checks=%llu badResult=%llu badKey=%llu)\n",
                     static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(g_g589Hits),
                     100.0 * static_cast<double>(g_g589Hits) / static_cast<double>(calls),
                     static_cast<unsigned long long>(g_g589Misses),
                     static_cast<unsigned long long>(g_g589VerifyChecks),
                     static_cast<unsigned long long>(g_g589VerifyBadResult),
                     static_cast<unsigned long long>(g_g589VerifyBadKey));
}

// Opened and closed by g260ExecuteAll / g260ExecutePrefix around their batch loops.
void g589BeginDrain()
{
    if (!g589Armed())
        return;
    if (g_g589Depth++ != 0)
        return; // nested: the inner scope shares the outer transaction and never re-arms
    g_g589BatchIndex = 0u;
    g_g589AtlasValid = false;
    g_g589PrevKeyValid = false;
    ++g_g589Drains;
}

void g589EndDrain()
{
    if (!g589Armed())
        return;
    if (--g_g589Depth != 0)
        return;
    g_g589AtlasValid = false;
    g_g589PrevKeyValid = false;
    if (g_g589BatchIndex > g_g589MaxBatches)
        g_g589MaxBatches = g_g589BatchIndex;
    g589Report();
}

// Called by g178TryFlushGpu once per batch, before any preparation. Records the batch's position
// in the transaction and, for the census, whether its atlas key is unchanged from the previous
// batch — the amortization headroom, measured with no behaviour change.
void g589NoteBatch()
{
    if (!g589Armed() || g_g589Depth == 0)
        return; // not inside a transaction (inline/legacy path): never memoize
    ++g_g589Batches;
    const uint32_t index = g_g589BatchIndex++;
    if (!g589CensusOn())
        return;
    const uint64_t key = g589AtlasKeyNow();
    if (index != 0u)
    {
        ++g_g589Later;
        if (g_g589PrevKeyValid && key == g_g589PrevKey)
            ++g_g589LaterSameKey;
    }
    g_g589PrevKey = key;
    g_g589PrevKeyValid = true;
}

// Disarm the memo for the remainder of this transaction. Called whenever a batch leaves the pure
// GPU path: a CPU band replay writes guest VRAM and the private depth mirror through paths that
// publish no residency metadata, so the key would not observe them. Fails CLOSED.
void g589DisarmTransaction()
{
    if (!g_g589AtlasValid)
        return;
    g_g589AtlasValid = false;
    ++g_g589Disarms;
}

// The memoized entry point. `real` is g310EnsureCurrent, passed by address so this file can sit
// after the atlas unit without the atlas unit needing this file's definitions.
bool g589MemoEnsureAtlas(bool (*real)())
{
    if (!g589DrainMemoOn() || g_g589Depth == 0)
    {
        if (g589StatOn())
            ++g_g589Misses;
        return real();
    }
    const uint64_t key = g589AtlasKeyNow();
    if (g_g589AtlasValid && key == g_g589AtlasKey)
    {
        ++g_g589Hits;
        if (g589VerifyOn())
        {
            // Same-run oracle. The claim under test is that calling the real function with an
            // unchanged input key is a no-op that returns the same value, so run it and check
            // both the value and that the key it leaves behind is still the memoized one.
            ++g_g589VerifyChecks;
            const bool got = real();
            if (got != g_g589AtlasResult)
                ++g_g589VerifyBadResult;
            if (g589AtlasKeyNow() != key)
                ++g_g589VerifyBadKey;
            g589Report();
            return g_g589AtlasResult;
        }
        g589Report();
        return g_g589AtlasResult;
    }
    ++g_g589Misses;
    const bool got = real();
    // Re-read the key AFTER the call: the real function legitimately advances its own lifecycle
    // flags (g_g310Initialized, refresh-pending), and memoizing under the pre-call key would make
    // the next batch compare against a key the state no longer has.
    g_g589AtlasKey = g589AtlasKeyNow();
    g_g589AtlasResult = got;
    g_g589AtlasValid = true;
    g589Report();
    return got;
}
