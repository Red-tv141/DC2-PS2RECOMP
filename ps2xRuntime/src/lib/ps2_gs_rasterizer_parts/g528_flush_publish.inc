// ===== G528: SCOPE THE FLUSH PROLOGUE TO THE BATCH'S OWN PAGE RANGES =====
//
// THE MEASUREMENT THAT LED HERE (G528 §1). G527 admitted five runtime-discovered render targets to
// deferral and routed them CPU-only inside g178TryFlushGpu. `[G290:probe]`, de-cumulated on Georama
// over 3,480 frames, then reads:
//
//   class                          count/f   ms/f    us each
//   GPU-accepted batch             13.96     6.553   469
//   GPU-REJECTED (rej=cpuonlyrtt)   5.01     7.591   1,515   <- pub 2.795 + replay 4.796
//
// 100% of the rejects are `cpuonlyrtt` on 0x181/0x18d/0x19f/0x1b7/0x1d7 — G527's own five. They
// carry 3.35 entries each and cost 3.2x an accepted batch. `pub` — the publication/materialize
// prologue the flush closure runs before the CPU band replay — is 2.795 ms/f over 5.01 batches,
// i.e. 558 us of PROLOGUE for a batch of three sprites.
//
// THE ARCHITECTURE DEFECT. Seven calls run before the CPU replay:
//
//   g278FlushPendingDepth(3)      publish EVERY pending GPU depth owner
//   g289MaterializeOwner(3)       materialize EVERY active copy owner
//   g272MaterializeAll(3)         materialize BOTH display colour waves
//   g277CensusHardEdge(3)         census (no state)
//   g276FlushPendingDisplay(3)    read back the pending DISPLAY wave
//   g261PrepareCpuReplay()        ranged already — per entry, the only scoped one
//   g242PrepareVramReadAll(vram)  sync ALL GPU-owned depth
//
// Five of the seven publish WHOLE-STATE. Every other consumer edge in this port was converted to
// ranged publication across G271-G289 — the upload edge (rasterizer_draw_sprite.inc:1501/1553/1570),
// the local-copy edge (:1744/:1798/:1852) and the inline-primitive edge all call the *ForRanges
// siblings. The flush prologue is the ONE edge that never was, because when it was written there
// was one batch per drain and "the whole frame" was the honest scope.
//
// Post-G527 that is false. A batch to fbp=0x181 (block 0x3020, fbw=2, a 128x128 scissor) cannot
// alias the display buffers at 0x0/0x68, and it forces a full display materialize + a display
// readback anyway. And the batch ALREADY CARRIES ITS EXACT RANGES: g260NoteAppend accumulates
// `b.wr` (colour + Z) and `b.rd` (texture + CLUT) into two G260RangeSets as it captures, from the
// same g144TargetRange / g254DepthSurface / g144TextureRange / g144ClutRange helpers that
// g261PrepareCpuReplay is already trusted to scope with. The flush closure throws that away.
//
// THE FIX (`scopedpub`). Publish what this batch actually touches:
//
//   src = b.rd  (what the CPU replay will READ)      dst = b.wr  (what it will WRITE)
//
// and call the already-proven ranged siblings once per span. The range set holds at most 8 merged
// spans, and the sibling predicates are disjunctions over (src, dst), so calling once per span with
// the other argument invalid is exactly equivalent to a single call over the union — with no
// bounding-box inflation, which is the whole point (a hull over "texture pages at 0x3820" and
// "target at 0x3020" would cover everything between them and publish the same whole state again).
//
// FAIL-CLOSED, three ways:
//   * either range set `unknown` (an entry whose rect could not be resolved)  -> whole-state path
//   * no ranges published at all (the pointers are null: a caller that is not
//     g260ExecutePrefix/g260ExecuteAll, e.g. the legacy immediate mid-flush)  -> whole-state path
//   * g242PrepareVramReadAll is NOT scoped. It has no range-set sibling (only a rect form,
//     g242SyncOverlappingDepth) and the census below prices it; leaving it whole-state keeps the
//     depth contract byte-identical while the four expensive publications get scoped.
//
// ARMS
//   DC2_G528_PUB=1        census: split the prologue into its 7 named calls + classify every
//                         executed batch by target class. Default-off. [G528:pub] / [G528:batch]
//   DC2_G528_SCOPEDPUB=1  the lever (bring-up default: OFF)
//   DC2_G528_NO_SCOPEDPUB=1  rollback once promoted
//   DC2_G528_VERIFY=1     ORACLE. Runs the scoped publish, then the whole-state publish, and counts
//                         how much work the whole-state pass still found to do. Any nonzero miss
//                         means the scoping skipped a publication that was required.
//   DC2_G528_NO_PUB=1     ⛔ CEILING PROBE, KNOWINGLY INCORRECT OUTPUT, NEVER PROMOTE. Skips the
//                         prologue entirely for CPU-replay batches, so the paired frame delta
//                         bounds every scoping lever that could ever be built here (G511's law:
//                         an inflation/deletion probe bounds the lever's gate). A deletion probe is
//                         valid on a CONSUMER edge and this is one (g492_producer_edge_deletion_probe).

namespace
{

static const bool g_g528PubStat = (std::getenv("DC2_G528_PUB") != nullptr);

// ⛔ ceiling probe — wrong pixels by construction.
static bool g528NoPubOn()
{
    static const bool on = envFlagEnabled("DC2_G528_NO_PUB");
    return on;
}

// PROMOTED DEFAULT-ON (G528 §6). Georama 36.223 -> 35.080, −1.143 ms/f = −3.15%, 4/4 paired;
// Dungeon 6 free-roam 42.190 -> 40.640, −1.550 = −3.67%, 2/2, against a control that repeats to
// 0.36 ms/f. Oracle `[G528:vfy] miss(wave=0 disp=0 depth=0 owner=0)` over 2,000 scoped batches;
// full-frame distribution 13/13 at exactly 211650, h=415, identical to the same-binary control.
// Rollback: DC2_G528_NO_SCOPEDPUB=1.
static bool g528ScopedPubOn()
{
    static const bool on = !envFlagEnabled("DC2_G528_NO_SCOPEDPUB") &&
                           !envFlagDisabled("DC2_G528_SCOPEDPUB");
    return on;
}

static bool g528VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G528_VERIFY");
    return on;
}

// ---------------------------------------------------------------------------------------------
// Census. thread_local + non-atomic: the flush prologue runs on ONE thread (the G144 flush-owner
// contract every function in it already relies on), and an atomic RMW per lap would be a
// self-inflicted cost on the exact bucket being priced (g526_draw_census.inc's rule).
enum G528Pub
{
    kG528Depth278 = 0, // g278FlushPendingDepth      — publish all pending GPU depth
    kG528Owner289,     // g289MaterializeOwner       — materialize all active copy owners
    kG528Disp272,      // g272MaterializeAll         — materialize both display colour waves
    kG528Hard277,      // g277CensusHardEdge         — census only
    kG528Pend276,      // g276FlushPendingDisplay    — display readback
    kG528Rtt261,       // g261PrepareCpuReplay       — already entry-ranged
    kG528Vram242,      // g242PrepareVramReadAll     — sync all GPU depth
    kG528PubN
};
static const char *const kG528PubName[kG528PubN] = {
    "d278", "o289", "m272", "h277", "p276", "r261", "v242"};

// Batch classes, so `pub` can be attributed to the population G527 created rather than guessed.
enum G528Cls
{
    kG528ClsDisplay = 0, // 0x0 / 0x68
    kG528ClsLegacy,      // the five g248TargetIndex addresses
    kG528ClsDiscovered,  // a G527 runtime-discovered target — the new population
    kG528ClsOther,
    kG528ClsN
};
static const char *const kG528ClsName[kG528ClsN] = {"disp", "legacy", "discov", "other"};

struct G528Acc
{
    uint64_t pubNs[kG528PubN];
    uint64_t pubN[kG528PubN];
    uint64_t clsNs[kG528ClsN];   // whole prologue, by batch class
    uint64_t clsN[kG528ClsN];
    uint64_t clsEntries[kG528ClsN];
    uint64_t spansWr, spansRd, unknownSets, nullSets, scoped, whole;
    uint64_t vfyBatches, vfyMissMat, vfyMissDisp, vfyMissDepth, vfyMissOwner;
};
static thread_local G528Acc t_g528{};

inline uint64_t g528Ns(std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

static void g528Report()
{
    double tot = 0.0;
    for (int i = 0; i < kG528PubN; ++i)
        tot += t_g528.pubNs[i] / 1e6;
    std::fprintf(stderr, "[G528:pub] totalMs=%.1f |", tot);
    for (int i = 0; i < kG528PubN; ++i)
        std::fprintf(stderr, " %s=%.1f/%llu", kG528PubName[i], t_g528.pubNs[i] / 1e6,
                     (unsigned long long)t_g528.pubN[i]);
    std::fprintf(stderr, " | scoped=%llu whole=%llu unknown=%llu null=%llu spans(wr=%llu rd=%llu)\n",
                 (unsigned long long)t_g528.scoped, (unsigned long long)t_g528.whole,
                 (unsigned long long)t_g528.unknownSets, (unsigned long long)t_g528.nullSets,
                 (unsigned long long)t_g528.spansWr, (unsigned long long)t_g528.spansRd);
    std::fprintf(stderr, "[G528:batch]");
    for (int i = 0; i < kG528ClsN; ++i)
        std::fprintf(stderr, " %s=%llu/%.1fms/%llue", kG528ClsName[i],
                     (unsigned long long)t_g528.clsN[i], t_g528.clsNs[i] / 1e6,
                     (unsigned long long)t_g528.clsEntries[i]);
    std::fprintf(stderr, "\n");
    if (g528VerifyOn())
        std::fprintf(stderr,
                     "[G528:vfy] batches=%llu miss(wave=%llu disp=%llu depth=%llu owner=%llu) "
                     "-- a miss is a still-pending GPU-owned region OVERLAPPING this batch's own "
                     "ranges; a merely-unflushed disjoint one is the saving, not a defect\n",
                     (unsigned long long)t_g528.vfyBatches,
                     (unsigned long long)t_g528.vfyMissMat,
                     (unsigned long long)t_g528.vfyMissDisp,
                     (unsigned long long)t_g528.vfyMissDepth,
                     (unsigned long long)t_g528.vfyMissOwner);
    std::fflush(stderr);
}

// The batch currently executing, published by g260ExecutePrefix / g260ExecuteAll (see the
// g_g528Wr / g_g528Rd declarations beside G260RangeSet in rasterizer_command_graph.inc). Null on
// any path that did not come through the graph — the fail-closed case.
static uint32_t g528ClassifyFbp(uint32_t fbp)
{
    if (fbp == 0x0u || fbp == 0x68u)
        return kG528ClsDisplay;
    if (g248TargetIndex(fbp) >= 0)
        return kG528ClsLegacy;
    if (g527DiscoveredTarget(fbp))
        return kG528ClsDiscovered;
    return kG528ClsOther;
}

// ---------------------------------------------------------------------------------------------
// THE ORACLE — and the one it replaces.
//
// ⚠️ G528's FIRST oracle asked "does the whole-state publish still find work after the scoped one?"
// and read `miss(disp=344 depth=344)` on 2,000 batches. That oracle was WRONG, not the lever: a
// disjoint batch leaving the display readback coalesced is `g276FlushPendingDisplayForRanges`'s
// DESIGNED behaviour (its own comment: "disjoint: keep the display readback coalesced"), i.e.
// exactly the saving being bought. "The whole-state path would still do something" and "the scoped
// path skipped something REQUIRED" are different propositions, and only the second is a defect.
//
// The safety property is precise: the CPU band replay reads and writes guest VRAM at exactly the
// pages in `b.rd` ∪ `b.wr` — that is what those sets are for, built by g260NoteAppend from the same
// g144TargetRange / g254DepthSurface / g144TextureRange / g144ClutRange helpers g261PrepareCpuReplay
// is already trusted to scope with. So a publication was required iff a region the GPU still owns
// INTERSECTS those pages. This oracle enumerates every still-pending GPU-owned region after the
// scoped publish and tests it against the batch's own sets. A hit is a real miss — the replay was
// about to touch bytes the GPU had not written back. Zero hits is proof of exactness for the run.
struct G528Miss
{
    uint32_t disp, depth, owner, wave;
};
static G528Miss g528VerifyResidual(const G260RangeSet &rd, const G260RangeSet &wr)
{
    G528Miss m{};
    const auto hits = [&](const G144BlockRange &r) {
        return r.valid && (rd.overlaps(r) || wr.overlaps(r));
    };
    if (s_g276Pend.active && hits(g277PendingDisplayRange()))
        ++m.disp;
    for (int i = 0; i < kG278PendSlots; ++i)
    {
        const G278PendDepth &p = s_g278PendTab[i];
        // privateMirror has no VRAM transfer consumer (g278FlushPendingDepthForRanges skips it for
        // the same reason), so it is not a publication the replay can depend on.
        if (p.active && !p.privateMirror && hits(g278PendingDepthRangeP(p)))
            ++m.depth;
    }
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        const G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        G144BlockRange r;
        r.first = o.firstOwnedBlock;
        r.last = o.lastOwnedBlock;
        r.valid = o.firstOwnedBlock <= o.lastOwnedBlock;
        // An owner whose range is UNRESOLVABLE is a miss by definition: nothing proved disjointness.
        if (!r.valid || hits(r))
            ++m.owner;
    }
    for (int i = 0; i < 2; ++i)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange r{};
        if (g272DirtyInfo(i, fbp, fbw, r) && (!r.valid || hits(r)))
            ++m.wave;
    }
    return m;
}

// The five whole-state publications, exactly as the flush closures ran them before G528.
static void g528PublishWhole(int cause)
{
    (void)g278FlushPendingDepth(cause);
    (void)g289MaterializeOwner(cause);
    g272MaterializeAll(cause);
    g277CensusHardEdge(cause);
    g276FlushPendingDisplay(cause);
}

// The scoped form. One call per span, `src` = read range, `dst` = write range. The sibling
// predicates are `(src.valid && overlaps(src)) || (dst.valid && overlaps(dst))`, i.e. disjunctions,
// so per-span calls with the other argument default-constructed (invalid) evaluate the union
// exactly — and every one of them is idempotent, because a published slot deactivates and a
// materialized wave clears its dirty flag, so a later span cannot re-publish it.
static void g528PublishScoped(int cause, const G260RangeSet &rd, const G260RangeSet &wr)
{
    const G144BlockRange none{};
    // G535 (DEFAULT-ON; kill DC2_G535_NO_EDGEPUB=1): the CPU band replay this prologue guards
    // reads AND writes the
    // G411 private depth mirror through g403DisplayZRead/g403DisplayZWrite, so it is a consumer of
    // any pending private-mirror owner. Every ranged publication below skips those owners because
    // they carry no VRAM transfer range — correct for transfers, wrong for the replay — which left
    // the replay depth-testing against a mirror the GPU had already superseded. Publish them here,
    // at the same semantic edge g278FlushPendingDepthForInline already uses for single primitives.
    // Range-scoping cannot apply: the mirror has no guest-VRAM range to intersect rd/wr against.
    if (g535EdgePubOn())
        for (int i = 0; i < kG278PendSlots; ++i)
            if (s_g278PendTab[i].active && s_g278PendTab[i].privateMirror)
                (void)g278FlushPendSlot(s_g278PendTab[i], cause);
    for (int i = 0; i < rd.n; ++i)
    {
        (void)g278FlushPendingDepthForRanges(cause, rd.r[i], none, false, false);
        g289MaterializeForRanges(rd.r[i], none, false, false, cause);
        g272MaterializeForRanges(rd.r[i], none, false, false, cause);
        g276FlushPendingDisplayForRanges(cause, rd.r[i], none, false, false);
    }
    for (int i = 0; i < wr.n; ++i)
    {
        (void)g278FlushPendingDepthForRanges(cause, none, wr.r[i], false, false);
        g289MaterializeForRanges(none, wr.r[i], false, false, cause);
        g272MaterializeForRanges(none, wr.r[i], false, false, cause);
        g276FlushPendingDisplayForRanges(cause, none, wr.r[i], false, false);
    }
    g277CensusHardEdge(cause);
}

// THE PROLOGUE. Replaces the seven-call block in both flush closures.
// `vram` is g_g144LastGs->m_vram; `fbp` is g_g144BlockFbp (for the class census only).
static void g528FlushPrologue(int cause, uint8_t *vram, uint32_t fbp)
{
    // ⛔ ceiling probe: delete the whole publication edge. Wrong pixels; bounds the lever.
    if (g528NoPubOn())
        return;

    const bool stat = g_g528PubStat;
    const auto t0 = stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto lap = [&](int which, std::chrono::steady_clock::time_point &mark) {
        if (!stat)
            return;
        const auto now = std::chrono::steady_clock::now();
        t_g528.pubNs[which] += g528Ns(mark, now);
        ++t_g528.pubN[which];
        mark = now;
    };
    auto mark = t0;

    const G260RangeSet *rd = g_g528Rd;
    const G260RangeSet *wr = g_g528Wr;
    const bool haveSets = (rd != nullptr && wr != nullptr);
    const bool usable = haveSets && !rd->unknown && !wr->unknown && (rd->n != 0 || wr->n != 0);
    const bool scope = g528ScopedPubOn() && usable;

    if (scope)
    {
        g528PublishScoped(cause, *rd, *wr);
        if (stat)
        {
            ++t_g528.scoped;
            t_g528.spansRd += static_cast<uint64_t>(rd->n);
            t_g528.spansWr += static_cast<uint64_t>(wr->n);
            // Booked to the four publication laps as one block: per-span laps would put 8x the
            // clock reads on the very edge being priced.
            lap(kG528Depth278, mark);
        }
        if (g528VerifyOn())
        {
            // ORACLE (see g528VerifyResidual): any GPU-owned region still pending AND intersecting
            // this batch's own ranges is a publication the scoping wrongly skipped. Then publish
            // whole-state anyway, so a verify run is never the arm that renders wrong pixels — the
            // oracle must be able to REPORT a miss without also causing it.
            const G528Miss m = g528VerifyResidual(*rd, *wr);
            ++t_g528.vfyBatches;
            t_g528.vfyMissDisp += m.disp;
            t_g528.vfyMissDepth += m.depth;
            t_g528.vfyMissOwner += m.owner;
            t_g528.vfyMissMat += m.wave;
            g528PublishWhole(cause);
        }
    }
    else
    {
        if (stat)
        {
            ++t_g528.whole;
            if (haveSets && (rd->unknown || wr->unknown))
                ++t_g528.unknownSets;
            if (!haveSets)
                ++t_g528.nullSets;
        }
        (void)g278FlushPendingDepth(cause);
        lap(kG528Depth278, mark);
        (void)g289MaterializeOwner(cause);
        lap(kG528Owner289, mark);
        g272MaterializeAll(cause);
        lap(kG528Disp272, mark);
        g277CensusHardEdge(cause);
        lap(kG528Hard277, mark);
        g276FlushPendingDisplay(cause);
        lap(kG528Pend276, mark);
    }

    g261PrepareCpuReplay();
    lap(kG528Rtt261, mark);
    g242PrepareVramReadAll(vram);
    lap(kG528Vram242, mark);

    if (stat)
    {
        const uint32_t cls = g528ClassifyFbp(fbp);
        t_g528.clsNs[cls] += g528Ns(t0, mark);
        ++t_g528.clsN[cls];
        t_g528.clsEntries[cls] += g_g144List.size();
        uint64_t n = 0;
        for (int i = 0; i < kG528ClsN; ++i)
            n += t_g528.clsN[i];
        if ((n % 2000ull) == 0ull)
            g528Report();
    }
}

} // namespace
