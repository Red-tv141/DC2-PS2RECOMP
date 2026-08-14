// ===== G592: THE PRIVATE-MIRROR PUBLICATION HAS NO CONSUMER TEST ================================
//
// THE GAP G591 NAMED.  G591 gave the G411 private depth mirror its WRITE half of an ownership
// record (a 16-band epoch bumped at the per-draw predicate).  Its READ half was left open, and
// G591 §8 measured it:
//
//   * `[G411:depth]` on `s05`: 7,040 publishes over 7,381 frames = **0.95 publications/frame**,
//     each a 416-row x 512 blocking GPU->CPU depth readback (852 KiB) followed by a 213 k-iteration
//     DOUBLE-PRECISION unpack that writes the mirror one bounds-checked pixel at a time.
//   * `[G299]` prices a readback job at 0.462 ms against 0.051-0.072 for every other class.
//   * ⭐⭐⭐ and on `s05`'s whole cutscene `[G591:zwhy] mirror(draws)` is FROZEN: **not one CPU draw
//     consumes the mirror for the entire window.**  Every one of those publications feeds nobody.
//
// WHY IT FIRES ANYWAY.  `g528PublishScoped` publishes EVERY active private-mirror owner
// unconditionally at every CPU-band-replay prologue (the G535 edge), on the stated ground that
// "range-scoping cannot apply: the mirror has no guest-VRAM range to intersect rd/wr against".
// That is true of VRAM RANGES and false of CONSUMERS.  The prologue runs on the flush-owner thread
// with `g_g144List` — the exact entry list the CPU band replay is about to execute — already in
// hand, and the only consumer the mirror has is a CPU draw that takes the `g403DisplayZ` path:
//
//     g403DisplayZ == g203UniversalZEnabled() && g404SharedZScope(fbp, fbw, zbpPage, zpsm)
//
// computed in rasterizer_draw_sprite.inc and rasterizer_draw_triangle.inc from `ctx` alone — the
// same `ctx` every G144Entry carries.  So "does this batch consume the mirror?" is decidable
// BEFORE the replay, from the captured entries, on one thread, with no VRAM range involved.
//
// ⚠️ G535 IS THE DEFECT THIS EDGE ALREADY CAUSED ONCE.  G535 is the game-wide missing-environment
// regression that came from exactly this consumer edge being unguarded, so:
//   1. CENSUS FIRST — [G592:pub] counts the population and every blocked conjunct BEFORE the gate
//      exists (g513), and it carries the INVARIANT below beside it.
//   2. FAIL OPEN — an empty list, a list this build cannot resolve, or universal-Z off all publish.
//   3. The safety property is an INVARIANT, not "the old path found no work" (g528): a CPU draw
//      must never take the `g403DisplayZ` path while a private-mirror owner is still pending.
//      `escapedReplay` MUST READ 0.  Under the control that is true because the publication
//      deactivates the owner; under the candidate it is true only if the consumer test is right.
//   4. The publication is NOT moved into g403DisplayZPrepare: the band replay runs on GSRowPool
//      lanes and the G278 pending table is flush-owner-thread-only.
//
// FLAGS
//   DC2_G592_CENSUS=1     [G592:pub] census + the invariant.  Behaviour-pure.
//   DC2_G592_CONSUMER=1   arm the gate (bring-up default: OFF)
//   DC2_G592_NO_CONSUMER=1   rollback once promoted
//   DC2_G419_AB=pubcons   paired within-process arm (hold 16)
// ==============================================================================================

namespace
{

static bool g592CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G592_CENSUS");
    return on;
}

static bool g592Paired()
{
    static const bool paired = []() {
        const char *v = std::getenv("DC2_G419_AB");
        return v != nullptr && std::strcmp(v, "pubcons") == 0;
    }();
    return paired;
}

// The A/B arm (-1 unless DC2_G419_AB=pubcons; 1 = candidate) is DEFINED AT GLOBAL SCOPE in
// g419_ab_instrument.inc, which this TU includes earlier — do NOT redeclare it inside this
// anonymous namespace or it names a different, never-defined symbol (the G568 linkage lesson).
// PROMOTED DEFAULT-ON (G592 §5).  s05 `blocked` −0.218 / −0.153, ridepod −0.150 / −0.448 —
// 4/4 runs favour the candidate, 190 of 285 windows, sign test z = 5.63, pooled −0.241 ms/f
// (t = −2.53); on `ridepod` the RAW deltas agree (−0.67% / −1.24%) because that route's arms are
// frame-balanced.  It deletes 8,448 of 17,152 blocking 416-row GPU→CPU depth readbacks per
// `ridepod` run (−49.3%) and 7,080 of 7,168 private-mirror publications on `s05`, with the
// `escapedReplay` invariant at 0 over 21.4 M mirror draws.  Rollback: DC2_G592_NO_CONSUMER=1.
static bool g592Selected()
{
    const int arm = g592PubConsArm();
    if (arm >= 0)
        return arm == 1;
    static const bool on = !envFlagEnabled("DC2_G592_NO_CONSUMER");
    return on;
}

// ⭐ ARM SYMMETRY (g520).  The CENSUS and the per-draw INVARIANT PROBE are arm-INDEPENDENT: both
// A/B buckets pay them, so the paired estimator above measures the SKIP and not the instrument.
// They stay DEFAULT-OFF in the shipped binary — the probe is one relaxed atomic per display-Z
// draw (2.4 k/frame on `ridepod`), which must not ride along with a promoted lever.
static bool g592Tracked()
{
    static const bool on = g592CensusOn() || g592Paired() ||
                           envFlagEnabled("DC2_G592_CONSUMER") ||
                           envFlagEnabled("DC2_G592_NO_CONSUMER");
    return on;
}

// ---------------------------------------------------------------------------------------------
// Census.  The prologue is flush-owner-thread-only, so those counters need no atomics; the
// invariant probe is bumped from the CPU raster paths, which run on GSRowPool lanes, so that one
// pair IS atomic.
struct G592Acc
{
    uint64_t prologues;    // prologues reached with >= 1 active private-mirror owner
    uint64_t owners;       // active private-mirror owners seen across them
    uint64_t consumer;     // owners whose batch DOES contain a display-Z entry -> publish
    uint64_t clean;        // owners whose batch does NOT -> skippable
    uint64_t skips;        // owners actually skipped (candidate arm only)
    uint64_t skipRows;     // depth rows not read back
    uint64_t cleanRows;    // depth rows the census says are skippable
    uint64_t blkEmpty;     // fail-open: the entry list was empty
    uint64_t blkNoUni;     // fail-open: universal-Z off, so the predicate cannot be evaluated
    uint64_t entriesSeen;  // entries scanned (the test's own cost denominator)
};
static G592Acc g_g592{};
static std::atomic<uint64_t> g_g592EscapedReplay{0};  // ⛔ INVARIANT — must read 0
static std::atomic<uint64_t> g_g592EscapedInline{0};  // context: the non-replay immediate path
static std::atomic<uint64_t> g_g592DisplayZDraws{0};  // display-Z draws seen at all

// True while at least one private-mirror owner is pending.  Read by the invariant probe on the
// raster lanes; the table is written only by the flush owner, and `active` is a plain bool that is
// cleared before the replay starts, so a lane can only ever observe a value that was already
// published by the same thread that starts the lanes.
static bool g592AnyPrivMirrorPending()
{
    for (int i = 0; i < kG278PendSlots; ++i)
        if (s_g278PendTab[i].active && s_g278PendTab[i].privateMirror)
            return true;
    return false;
}

// ⛔ THE INVARIANT.  Called from the sprite and triangle paths at the `g403DisplayZ` predicate —
// the predicate, not the write sites, and NOT `&& zWrite`: a depth TEST reads the mirror too.
static inline void g592NoteDisplayZDraw(bool displayZ)
{
    if (!displayZ || !g592Tracked())
        return;
    g_g592DisplayZDraws.fetch_add(1u, std::memory_order_relaxed);
    if (!g592AnyPrivMirrorPending())
        return;
    if (t_g144InReplay)
        g_g592EscapedReplay.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g592EscapedInline.fetch_add(1u, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// THE CONSUMER TEST.  One pass over the batch's captured entries, evaluating the same predicate
// the draw paths will evaluate.  Fails OPEN on every uncertainty.
enum G592Verdict
{
    kG592Consumer = 0,  // a display-Z entry is present -> publish
    kG592Clean,         // no entry can reach the mirror -> skippable
    kG592OpenEmpty,     // no entries -> publish
    kG592OpenNoUni      // universal-Z off -> publish
};

static G592Verdict g592ClassifyBatch()
{
    if (!g203UniversalZEnabled())
        return kG592OpenNoUni;
    if (g_g144List.empty())
        return kG592OpenEmpty;
    const bool tracked = g592Tracked();
    for (const G144Entry &e : g_g144List)
    {
        if (tracked)
            ++g_g592.entriesSeen;
        if (g404SharedZScope(e.ctx.frame.fbp, e.ctx.frame.fbw,
                             static_cast<uint32_t>(e.ctx.zbuf & 0x1FFu),
                             static_cast<uint32_t>((e.ctx.zbuf >> 24) & 0xFu)))
            return kG592Consumer;
    }
    return kG592Clean;
}

static void g592Report()
{
    if (!g592Tracked())
        return;
    std::fprintf(stderr,
                 "[G592:pub] prologues=%llu owners=%llu consumer=%llu CLEAN=%llu (%.2f%% SKIPPABLE) "
                 "skips=%llu skipRows=%llu cleanRows=%llu | openFail(empty=%llu noUni=%llu) "
                 "entries=%llu | mirrorDraws=%llu escaped(replay=%llu inline=%llu)\n",
                 (unsigned long long)g_g592.prologues, (unsigned long long)g_g592.owners,
                 (unsigned long long)g_g592.consumer, (unsigned long long)g_g592.clean,
                 g_g592.owners > 0u ? 100.0 * (double)g_g592.clean / (double)g_g592.owners : 0.0,
                 (unsigned long long)g_g592.skips, (unsigned long long)g_g592.skipRows,
                 (unsigned long long)g_g592.cleanRows,
                 (unsigned long long)g_g592.blkEmpty, (unsigned long long)g_g592.blkNoUni,
                 (unsigned long long)g_g592.entriesSeen,
                 (unsigned long long)g_g592DisplayZDraws.load(std::memory_order_relaxed),
                 (unsigned long long)g_g592EscapedReplay.load(std::memory_order_relaxed),
                 (unsigned long long)g_g592EscapedInline.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// THE EDGE.  Replaces the unconditional G535 private-mirror publication loop in
// g528PublishScoped.  Returns nothing: it either publishes an owner or deliberately leaves it
// pending, and a left-pending owner is still published by every other edge that needs it
// (g278FlushPendingDepth, g278FlushPendingDepthForInline, the same-key wave, the frame boundary).
//
// ⭐ CENSUS BEFORE GATE (g513): under `tracked` the classification and every counter run whether
// or not the gate is armed, so the CLEAN share and the blocked conjuncts are measured on the
// CONTROL arm too — that is how the 99.83% / 48.21% split between the two routes was established
// before a single publication was skipped.
static void g592PublishPrivateMirrors(int cause)
{
    const bool tracked = g592Tracked();
    const bool gate = g592Selected();
    if (!tracked && !gate)
    {
        // ROLLBACK path (DC2_G592_NO_CONSUMER=1), byte-identical to G535's unconditional loop.
        for (int i = 0; i < kG278PendSlots; ++i)
            if (s_g278PendTab[i].active && s_g278PendTab[i].privateMirror)
                (void)g278FlushPendSlot(s_g278PendTab[i], cause);
        return;
    }

    bool any = false;
    for (int i = 0; i < kG278PendSlots && !any; ++i)
        any = s_g278PendTab[i].active && s_g278PendTab[i].privateMirror;
    if (!any)
        return;
    ++g_g592.prologues;

    const G592Verdict v = g592ClassifyBatch();
    if (v == kG592OpenEmpty)
        ++g_g592.blkEmpty;
    else if (v == kG592OpenNoUni)
        ++g_g592.blkNoUni;

    for (int i = 0; i < kG278PendSlots; ++i)
    {
        G278PendDepth &p = s_g278PendTab[i];
        if (!p.active || !p.privateMirror)
            continue;
        ++g_g592.owners;
        const int rows = (p.rowHi >= p.rowLo) ? (p.rowHi - p.rowLo + 1) : 0;
        if (v == kG592Clean)
        {
            ++g_g592.clean;
            g_g592.cleanRows += static_cast<uint64_t>(rows);
            if (gate)
            {
                ++g_g592.skips;
                g_g592.skipRows += static_cast<uint64_t>(rows);
                continue; // the readback + unpack this batch cannot consume
            }
        }
        else
        {
            ++g_g592.consumer;
        }
        (void)g278FlushPendSlot(p, cause);
    }

    // Cadence, not a frame hook: this part is included after rasterizer_gpu_alias_page_view.inc,
    // so g261Report cannot reach it. Every figure is cumulative, so the LAST line of a run is the
    // whole-run total; the early prints exist so a short bring-up run still emits one.
    const uint64_t n = g_g592.prologues;
    if (n == 1ull || n == 64ull || (n % 1024ull) == 0ull)
        g592Report();
}

} // namespace
