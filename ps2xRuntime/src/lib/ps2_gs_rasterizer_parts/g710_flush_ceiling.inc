// ==============================================================================================
// G710 — FIXED FLUSH-PREPARATION CEILING PROBE (reconnaissance only)
// ==============================================================================================
//
// ⛔⛔ THIS FILE IS UNREFERENCED AND IS NOT COMPILED. Nothing `#include`s it, no CMake option
// defines `PS2X_G710_DIAG`, and it reaches no binary. It was unwired under Rule 31 when the
// direction closed, so the shipping source relinks to the deliverable byte for byte.
//
// ⚠️ It is retained OUT of the build on purpose, and re-wiring it is not free: even `#if`-excluded
// with `constexpr` stubs — every one of its 78 use sites folding away — its mere source presence in
// `ps2_gs_rasterizer.cpp` measured **+0.362 ms/f pooled (t = 4.48, 0/3 blocks negative)**, larger
// than most levers this project ever shipped. Compile-time-dead text is not free in a hot TU.
//
// TO RE-WIRE: restore `option(PS2X_G710_DIAG ...)` + the scoped COMPILE_DEFINITIONS block in
// PS2Recomp/ps2xRuntime/CMakeLists.txt, the `#include` in ps2_gs_rasterizer.cpp, and the call sites
// listed in plans/phase-G711-fix-log.md §8. See FINAL_PROJECT_STATE.md §7 before doing so.
//
// WHY THIS EXISTS. Every number the roadmap's "9.6 ms/f of fixed flush preparation" thesis rests on
// (`[G523:fit]`, `[G298:profile]`, `[G323:census]`, `[G654:layer]`) is produced by a binary built
// with PS2X_G684_HOT_DIAG or PS2X_G654_DIAG. **Rule 79** says a leaf priced on a HOT_DIAG binary is
// not a ship-binary prize — G707 built an exact hoist against a 1.4 ms/f diag estimate and moved
// ship GS own by −0.010. Rule 12b is the mechanism: extra code in this TU regresses the whole TU,
// and that applies to the ESTIMATE as much as to the lever.
//
// So before this phase designs anything, it prices the same passes by REMOVAL on a binary whose
// shipping configuration is intact: one binary, two env arms, and the difference is real frame time
// on the real hot set. If amputating `deps` + `tex` + the per-entry classifier does not move the
// frame by multiple milliseconds, the whole "fixed preparation" model is a diagnostic artefact and
// the phase must pivot — that is a result, not a failure.
//
// ⚠️ THIS PROBE IS CORRUPTING. Skipping the bind pre-pass denies every FBO source; skipping the
// texture pass uploads nothing; broadcasting entry 0's classification is wrong wherever GS state
// moves inside a batch. Pixels WILL be wrong on the armed arms. Under Rule 42 a corrupting ceiling
// probe must ship its own ADMISSIBILITY GATE and it must be read BEFORE the delta:
// `[G710:amp]` publishes the workload — flushes, entries, submitted draws, native fallbacks and
// rendered frames — so an arm that converted WORK into a DIFFERENT WORKLOAD (a batch pushed to CPU
// replay, a draw dropped) is visible as such and its delta is discarded. Rule 23/68: a number from
// a broken prototype is reconnaissance, never a claimed saving.
//
// The counters are PER FLUSH, never per entry (Rule 63: an instrument whose cost scales with a
// count the lever changes must not be armed in the timing arm). Four relaxed atomic adds against
// ~50 flushes/frame is ~200 ops/frame and is paid identically by both arms.
//
//   DC2_G710_SKIP=<mask>   bit 0x01  deps  — skip the whole G261 FBO-bind pre-pass
//                          bit 0x02  tex   — skip the whole per-entry texture preparation pass
//                          bit 0x04  class — classify entry 0 only, broadcast to the whole list
//                          bit 0x08  mir   — skip g264FlushMirrorsAtFlush (`pro1mir`)
//   DC2_G710_STAT=1        print `[G710:amp]` every 6000 flushes
//
// Accepts decimal or 0x-prefixed hex. Nothing here is promotable; the mask is 0 by default and the
// whole file is dead code on a shipping run except for the four counters.

// ⚠️ RULE 12c / RULE 31: everything below is DIAGNOSTIC and is compiled out of the deliverable.
// Build a measuring binary with -DPS2X_G710_DIAG=ON in an OTHERWISE SHIPPING configuration - that
// is the whole point of this instrument (Rule 79: a per-pass number is only a ship-binary cost if
// the binary that produced it was the shipping build). With the option OFF every arm below is a
// `constexpr false` / empty inline, so all call sites in g178TryFlushGpu vanish.
#if defined(PS2X_G710_DIAG)

static uint32_t g710SkipMask()
{
    static const uint32_t mask = [] {
        const char *v = std::getenv("DC2_G710_SKIP");
        if (v == nullptr || *v == '\0')
            return 0u;
        return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    }();
    return mask;
}

static constexpr uint32_t kG710SkipDeps = 0x1u;
static constexpr uint32_t kG710SkipTex = 0x2u;
static constexpr uint32_t kG710SkipClass = 0x4u;
static constexpr uint32_t kG710SkipMir = 0x8u;

static bool g710StatOn()
{
    static const bool on = envFlagEnabled("DC2_G710_STAT");
    return on;
}

// ===== G710 BALLAST — the instrument that REPLACED amputation ================================
//
// ⛔ AMPUTATION IS REFUTED AS AN INSTRUMENT HERE. `DC2_G710_SKIP=3` passed the workload gate over
// the first 24,000 flushes and then, deeper into the route, made the frame **135 ms/f**: the
// candidate arm reached `[G154:perf] frame=3839` against the control's `13271` (Rule 76), with
// `g144 upload` at **127 ms/f against 12.45**. Deleting texture preparation does not remove work —
// it makes the runtime redo far more of it somewhere else. A preparation pass with a CONSUMER
// cannot be priced by removing it.
//
// ⭐ BALLAST prices it the other way: run the pass ONE EXTRA TIME and measure the increment. The
// render path is untouched, every decision is already made, the workload is identical by
// construction, and the increment IS the cost of one redundant traversal — which is exactly the
// quantity a "collapse the scans / classify once per state run" architecture can remove.
//
//   DC2_G710_BALLAST=<n>       extra passes (default 0)
//   DC2_G710_BALLAST_MASK=<m>  0x1 deps (read-only tail recompute), 0x2 tex, 0x4 class
//
// The repeat passes are provably side-effect-free:
//   tex   — every key is already in `s_batchKeys`, so every entry takes the `count()` -> continue
//           branch; no decode, hash, registration or upload can be reached a second time.
//   class — classifies into a SCRATCH state, never into `s_states`.
//   deps  — recomputes only the read-only tail terms (`g144TextureRange` / `g144ClutRange` /
//           `g144RangeOverlaps` / `g291PagesCurrent`) against a snapshot of the dirty set taken
//           BEFORE the real loop, so the population matches even after a materialize cleared it.
//           It never materializes, never binds, never writes `s_g261FboSrc`.
// Each sinks into one atomic per flush so nothing can be optimised away.
static uint32_t g710Ballast()
{
    static const uint32_t n = [] {
        const char *v = std::getenv("DC2_G710_BALLAST");
        if (v == nullptr || *v == '\0')
            return 0u;
        return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    }();
    return n;
}

static uint32_t g710BallastMask()
{
    static const uint32_t m = [] {
        const char *v = std::getenv("DC2_G710_BALLAST_MASK");
        if (v == nullptr || *v == '\0')
            return 0x7u; // all three unless told otherwise
        return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    }();
    return m;
}

static constexpr uint32_t kG710BalDeps = 0x1u;
static constexpr uint32_t kG710BalTex = 0x2u;
static constexpr uint32_t kG710BalClass = 0x4u;

static std::atomic<uint64_t> g_g710BallastSink{0};

// ===== G710 SHIP-CONFIG PER-FLUSH STAGE DECOMPOSITION (DC2_G710_STAGES=1) =====================
//
// ⛔ THE PROBLEM THIS SOLVES. Every per-pass number the roadmap steers by comes from
// `[G523:fit]` / `[G298:profile]` / `[G323:census]` / `[G654:layer]`, and **none of those flags
// exists in the shipped binary**:
//
//   grep -c on build64/Release/dc2_runner.exe
//     DC2_G298_PROFILE  0     DC2_G523_FIT  0     DC2_G323_CENSUS  0
//     DC2_G576_FBZSPLIT 1     DC2_G581_CLSSPLIT 1
//
// ⛔⛔ And the last two are a **Rule 69 dead instrument in the deliverable**: `s_g576FbzSplit` and
// `s_g581ClsSplit` are ordinary runtime bools compiled into the ship binary, so arming them pays
// for three and four `steady_clock::now()` calls per flush — but BOTH reporters live inside
// `if (s_g298Profile)`, which is `static constexpr bool = false` in that build. On a shipping
// binary those two flags buy timestamps that can never be printed. (Rule 69 asks for a CALL SITE;
// this is its sibling — a call site that exists but is provably unreachable.)
//
// ⭐ So this is the replacement: the same seams, armed by ONE flag, reported from code that is not
// compile-excluded, in the shipping build configuration — which is what Rule 79 requires before a
// per-pass number may be converted into a ship-binary prize.
//
// WINDOWED, NOT CUMULATIVE. `[G273]`/`[G298]`/`[G523]`/`[G576]` all print cumulative process means,
// which is why G704 had to warn that a mid-route snapshot and the final one differ materially.
// This resets every window, so each line is a true mean over its own flushes (Rule 72).
//
// COST: 15 clock reads per flush, ~50 flushes/frame => ~750/frame ~= 15 us/f. Diagnostic runs only.

static bool g710StagesOn()
{
    static const bool on = envFlagEnabled("DC2_G710_STAGES");
    return on;
}

static constexpr int kG710Marks = 15;
// 0 flush start | 1 pro0trn | 2 pro1mir | 3 pro2fbw | 4 pro3set | 5 class | 6 fbzAlias
// 7 fbzDepth | 8 fbzClassify | 9 fbzPack | 10 deps | 11 tex | 12 verts | 13 submit | 14 post
static const char *const kG710MarkName[kG710Marks - 1] = {
    "pro0trn", "pro1mir", "pro2fbw", "pro3set", "class",
    "fbzAlias", "fbzDepth", "fbzCls", "fbzPack",
    "deps", "tex", "verts", "submit", "post"};

struct G710StageAcc
{
    uint64_t n = 0;
    uint64_t entries = 0;
    uint64_t draws = 0;
    uint64_t uploads = 0;
    uint64_t ns[kG710Marks - 1]{};
};
static G710StageAcc g_g710Stage;

static void g710NoteStages(const std::chrono::steady_clock::time_point *marks, uint64_t entries,
                           uint64_t draws, bool uploadFb)
{
    G710StageAcc &a = g_g710Stage;
    ++a.n;
    a.entries += entries;
    a.draws += draws;
    a.uploads += uploadFb ? 1u : 0u;
    for (int i = 0; i + 1 < kG710Marks; ++i)
        a.ns[i] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(marks[i + 1] - marks[i]).count());
    if ((a.n % 3000u) != 0u)
        return;
    const double d = static_cast<double>(a.n);
    uint64_t sum = 0u;
    for (int i = 0; i + 1 < kG710Marks; ++i)
        sum += a.ns[i];
    std::fprintf(stderr, "[G710:stages] flushes=%llu entries/flush=%.1f draws/flush=%.1f "
                         "uploadFb=%.1f%% totalMsPerFlush=%.4f |",
                 static_cast<unsigned long long>(a.n), static_cast<double>(a.entries) / d,
                 static_cast<double>(a.draws) / d,
                 100.0 * static_cast<double>(a.uploads) / d,
                 static_cast<double>(sum) / (1.0e6 * d));
    for (int i = 0; i + 1 < kG710Marks; ++i)
        std::fprintf(stderr, " %s=%.4f", kG710MarkName[i],
                     static_cast<double>(a.ns[i]) / (1.0e6 * d));
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    a = G710StageAcc{}; // WINDOWED: reset so the next line is its own mean, not a running one
}

// ===== G710 STATE-RUN POPULATION CENSUS (DC2_G710_CENSUS=1) ==================================
//
// The architecture this phase tests is "classify once per STATE RUN, not once per entry", so the
// number that decides whether it is worth building is RUNS PER FLUSH. G510 measured adjacent GS
// state 92.80% identical, but Rule 85 says a percentage is a claim about the population that was
// running when it was sampled — re-count it on today's binary before designing against it.
//
// ⚠️ COSTS PER ENTRY. Rule 63: never arm this in a timing arm. It is a separate run.
//
// `runs`      entries whose GS state differs from their predecessor's (G510's own memcmp chain)
// `dirty`     resident targets marked dirty when the bind pre-pass opened (G663 measured <= 2/14)
// `bindEnt`   entries that entered the bind pre-pass loop
// `tail`      (entry x dirty-target) pairs that reached the expensive tail body — the UV-extent
//             test, g144TextureRange / g144ClutRange, g291PagesCurrent, materialize
// `texEnt`    entries that entered the texture preparation loop
// `texKeys`   DISTINCT effective texture keys in the flush (s_batchKeys.size())
static bool g710CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G710_CENSUS");
    return on;
}

// ⭐ AND the per-flush FIXED cost hypothesis. `[G523:fit]` books `tex` at 91% FIXED — a cost that
// does not scale with entry count at all. The pass owns two `std::unordered_set<uint64_t>`s that
// are CLEARED once per flush (`s_batchKeys`, `s_g310BuiltViews`). MSVC's `unordered_set::clear()`
// is O(BUCKET_COUNT), not O(size): it walks the whole bucket array resetting it. A set that grew
// its bucket array once, on one unusual flush, pays that walk on EVERY flush thereafter, forever,
// with one element in it — which is exactly the shape of a 100%-fixed per-flush cost, and exactly
// what `texKeys/flush = 1.00` makes absurd. So the census also publishes the BUCKET COUNTS.
static std::atomic<uint64_t> g_g710CenFlushes{0}, g_g710CenEntries{0}, g_g710CenRuns{0},
    g_g710CenDirty{0}, g_g710CenBindEnt{0}, g_g710CenTail{0}, g_g710CenTexEnt{0},
    g_g710CenTexKeys{0}, g_g710CenDraws{0}, g_g710CenTexBuckets{0}, g_g710CenViewBuckets{0},
    g_g710CenTexBucketsMax{0}, g_g710CenViewBucketsMax{0};

static void g710NoteBuckets(uint64_t texBuckets, uint64_t viewBuckets)
{
    g_g710CenTexBuckets.fetch_add(texBuckets, std::memory_order_relaxed);
    g_g710CenViewBuckets.fetch_add(viewBuckets, std::memory_order_relaxed);
    uint64_t m = g_g710CenTexBucketsMax.load(std::memory_order_relaxed);
    while (texBuckets > m &&
           !g_g710CenTexBucketsMax.compare_exchange_weak(m, texBuckets, std::memory_order_relaxed))
    {
    }
    m = g_g710CenViewBucketsMax.load(std::memory_order_relaxed);
    while (viewBuckets > m &&
           !g_g710CenViewBucketsMax.compare_exchange_weak(m, viewBuckets,
                                                          std::memory_order_relaxed))
    {
    }
}

static void g710NoteCensus(uint64_t entries, uint64_t runs, uint64_t dirty, uint64_t bindEnt,
                           uint64_t tail, uint64_t texEnt, uint64_t texKeys, uint64_t draws)
{
    const uint64_t n = g_g710CenFlushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g710CenEntries.fetch_add(entries, std::memory_order_relaxed);
    g_g710CenRuns.fetch_add(runs, std::memory_order_relaxed);
    g_g710CenDirty.fetch_add(dirty, std::memory_order_relaxed);
    g_g710CenBindEnt.fetch_add(bindEnt, std::memory_order_relaxed);
    g_g710CenTail.fetch_add(tail, std::memory_order_relaxed);
    g_g710CenTexEnt.fetch_add(texEnt, std::memory_order_relaxed);
    g_g710CenTexKeys.fetch_add(texKeys, std::memory_order_relaxed);
    g_g710CenDraws.fetch_add(draws, std::memory_order_relaxed);
    if ((n % 6000u) != 0u)
        return;
    const double d = static_cast<double>(n);
    const auto ld = [](std::atomic<uint64_t> &a) {
        return static_cast<double>(a.load(std::memory_order_relaxed));
    };
    const double ent = ld(g_g710CenEntries);
    std::fprintf(stderr,
                 "[G710:runs] flushes=%llu entries/f=%.1f runs/f=%.2f runShare=%.2f%% "
                 "dirty/f=%.2f bindEnt/f=%.1f tail/f=%.1f texEnt/f=%.1f texKeys/f=%.2f "
                 "draws/f=%.1f entriesPerRun=%.2f | buckets(tex=%.1f max=%llu view=%.1f "
                 "max=%llu)\n",
                 static_cast<unsigned long long>(n), ent / d, ld(g_g710CenRuns) / d,
                 ent > 0.0 ? 100.0 * ld(g_g710CenRuns) / ent : 0.0, ld(g_g710CenDirty) / d,
                 ld(g_g710CenBindEnt) / d, ld(g_g710CenTail) / d, ld(g_g710CenTexEnt) / d,
                 ld(g_g710CenTexKeys) / d, ld(g_g710CenDraws) / d,
                 ld(g_g710CenRuns) > 0.0 ? ent / ld(g_g710CenRuns) : 0.0,
                 ld(g_g710CenTexBuckets) / d,
                 static_cast<unsigned long long>(
                     g_g710CenTexBucketsMax.load(std::memory_order_relaxed)),
                 ld(g_g710CenViewBuckets) / d,
                 static_cast<unsigned long long>(
                     g_g710CenViewBucketsMax.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

static std::atomic<uint64_t> g_g710Flushes{0};
static std::atomic<uint64_t> g_g710Entries{0};
static std::atomic<uint64_t> g_g710Draws{0};
static std::atomic<uint64_t> g_g710Verts{0};

// Called once per flush that reaches the submit seam, with the workload this flush actually
// produced. This is the admissibility gate, not a timer.
static void g710NoteFlush(uint64_t entries, uint64_t draws, uint64_t verts)
{
    const uint64_t n = g_g710Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g710Entries.fetch_add(entries, std::memory_order_relaxed);
    g_g710Draws.fetch_add(draws, std::memory_order_relaxed);
    g_g710Verts.fetch_add(verts, std::memory_order_relaxed);
    if (!g710StatOn() || (n % 6000u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G710:amp] mask=0x%x flushes=%llu entries=%llu draws=%llu verts=%llu "
                 "entriesPerFlush=%.1f drawsPerFlush=%.1f\n",
                 g710SkipMask(), static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_g710Entries.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g710Draws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g710Verts.load(std::memory_order_relaxed)),
                 static_cast<double>(g_g710Entries.load(std::memory_order_relaxed)) /
                     static_cast<double>(n),
                 static_cast<double>(g_g710Draws.load(std::memory_order_relaxed)) /
                     static_cast<double>(n));
    std::fflush(stderr);
}

#else // !PS2X_G710_DIAG  -- inert stubs so every call site compiles to nothing

static constexpr uint32_t kG710SkipDeps = 0x1u;
static constexpr uint32_t kG710SkipTex = 0x2u;
static constexpr uint32_t kG710SkipClass = 0x4u;
static constexpr uint32_t kG710SkipMir = 0x8u;
static constexpr uint32_t kG710BalDeps = 0x1u;
static constexpr uint32_t kG710BalTex = 0x2u;
static constexpr uint32_t kG710BalClass = 0x4u;
static constexpr int kG710Marks = 15;

// The ballast sink is referenced by a branch the compiler proves dead (`g710BalSink` can only be
// zero once g710Ballast() is constexpr 0), but the NAME must still resolve.
static std::atomic<uint64_t> g_g710BallastSink{0};

static constexpr uint32_t g710SkipMask() { return 0u; }
static constexpr uint32_t g710Ballast() { return 0u; }
static constexpr uint32_t g710BallastMask() { return 0u; }
static constexpr bool g710StatOn() { return false; }
static constexpr bool g710CensusOn() { return false; }
static constexpr bool g710StagesOn() { return false; }
static inline void g710NoteFlush(uint64_t, uint64_t, uint64_t) {}
static inline void g710NoteBuckets(uint64_t, uint64_t) {}
static inline void g710NoteCensus(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t) {}
static inline void g710NoteStages(const std::chrono::steady_clock::time_point *, uint64_t, uint64_t,
                                  bool) {}

#endif // PS2X_G710_DIAG
