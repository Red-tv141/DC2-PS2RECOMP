// ===== G603: WHY DOES A CPU-FALLBACK BATCH FALL BACK — ANSWERED FOR *EVERY* CLASS =====
// (DEFAULT-OFF: DC2_G603_ADMIT=1. Nothing in this file runs, allocates or branches otherwise.)
//
// THE MEASUREMENT THAT MADE THIS NECESSARY (route `s03`, the user's 2026-08-16 recording).
// `[G290:probe]` de-cumulated per window says Layer 1's cost on that route is not the graph
// machinery at all — it is the CPU BAND REPLAY inside it:
//
//   window                         gpuOk    replay   pub     frame
//   n=3391..3871 (VU1-poled)      12.679    0.000   0.000    31 ms/f
//   n=1111..1261                  10.497   23.017   1.281    44
//   n=1531..1831                  15.747   17.004   1.097    45-55
//   n=2491..2611 (the SPIKE)      10.652   48.115   1.526    50/156/140/77/40
//
// `[G529:cls]` splits that replay by target class, and the three populations are disjoint:
// `discov` 23.4 s (G527-discovered targets), `disp` 18.6 s (0x0/0x68) and `legacy` 7.1 s (0x139)
// of 49.1 s cumulative.
//
// ⭐⭐⭐ THE LAW THIS PHASE BOUGHT. `[G583:rejcensus]` ranks the fallback population by ENTRIES
// (g568_rank_fallback_by_entries_not_events). On `s03` that ranking is WRONG: the display batches
// are 1.7% of entries and 122.8 ms/f of the 156 ms/f spike, because a display entry carries ~31,000
// replayed pixels and a discovered-target entry carries ~6,000. **Entries is a proxy for the work
// only while entry SIZE is constant.** Rank by ms (`[G529:cls] wall`) or by px, and read the entry
// census as the shape lookup it actually is.
//
// WHAT THIS CENSUS ADDS THAT NEITHER EXISTING ONE CAN ANSWER.
//   * `[G583:rejcensus]` names the reason the batch was REJECTED. For `cpuonlyrtt` that reason is
//     an early return taken BEFORE the classifier ever ran (rasterizer_vram_materialization.inc's
//     g527DiscoveredTarget / g248TargetIndex guards), so it says nothing about whether the entries
//     are GPU-admissible. Every discovered target in this port is in that state: unmeasured.
//   * `[G290:gpufail]` prints ENTRY 0, which is the rejecter only by coincidence.
//   * Neither answers "if I fixed the FIRST failing conjunct, would the batch then be admitted?" —
//     and that is the only question a lever design depends on (g534_admission_proof_true_but_
//     insufficient: an admission proof answers one question; admission has many).
//
// So: at the single unscoped fallback choke point (beside g583NoteFallback), run the REAL
// classifier over every entry of the batch, twice —
//   pass 1: as shipped                        -> per-entry first-failing site
//   pass 2: with the blend reject relaxed     -> what the NEXT failing site would be
// — and book, per (class, fbp): batches, entries, how many batches would classify CLEAN, the site
// histogram, and the ALPHA register shapes of the blend rejects so a new blend mode can be
// designed from the census rather than from entry 0.
//
// ⚠️ The dry-run calls the shipped g178ClassifyEntry, so it also bumps that function's own
// g_g178Reject{State,Blend,Tex} counters and clobbers g_g266ClassifySite. Both are read-only
// diagnostics, this hook runs strictly AFTER g583NoteFallback has consumed the site, and the whole
// file is default-off — but do not run this census in the same process as a run whose numbers you
// intend to quote from those counters.
//
// ⚠️ It is a CENSUS, not a timing instrument. It doubles classification work for every fallback
// batch, so `[G146:frame]` under DC2_G603_ADMIT=1 is meaningless (g5xx: a census that moves what it
// measures). Read `[G529:cls]` from a separate, un-armed run.

namespace
{

static bool g603AdmitCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G603_ADMIT");
    return on;
}

// site index kG262Rej* (0..7), or kG603SiteOk for an entry the classifier accepted.
static constexpr int kG603SiteOk = kG262RejSiteCount; // 8
static constexpr int kG603SiteN = kG262RejSiteCount + 1;
static const char *const kG603SiteName[kG603SiteN] = {
    "frame", "atest", "scissor", "blend", "tex", "zpsm", "depth", "legacyz", "ok"};

struct G603Row
{
    uint64_t batches = 0, entries = 0;
    uint64_t batchesAllOk = 0;      // every entry classified clean AS SHIPPED
    uint64_t batchesAllOkRelax = 0; // ...and with the blend reject relaxed
    // G604: the same question for the `discov` population, whose first failing conjunct is DEPTH
    // on ~100% of entries. A first failing conjunct is not an admission proof (g534), so measure
    // what comes next instead of assuming nothing does.
    uint64_t batchesAllOkRelaxZ = 0;    // ...with the DEPTH reject relaxed
    uint64_t batchesAllOkRelaxBoth = 0; // ...with depth AND blend relaxed
    uint64_t batchesShapeOk = 0;    // the G586/G587 transient shape contract holds
    uint64_t site[kG603SiteN] = {};
    uint64_t siteRelax[kG603SiteN] = {};
    uint64_t siteRelaxZ[kG603SiteN] = {};
    uint64_t siteRelaxBoth[kG603SiteN] = {};
    uint64_t primMask = 0;
    uint64_t texturedEntries = 0;
    uint64_t guestZEntries = 0;   // zte && ztst!=ALWAYS, i.e. reads real Z
    uint64_t zWriteEntries = 0;
    uint32_t zbpSeen = 0xFFFFu;   // last non-trivial ZBP page, for the shared-Z contract
    uint32_t zpsmSeen = 0xFFu;
    uint32_t fbwSeen = 0u;
};

// key = cls << 9 | fbp   (fbp is 9 bits; cls is g528ClassifyFbp's 0..3)
static std::map<uint32_t, G603Row> g_g603Rows;
// key = abcd << 9 | (fix==128) << 8 | tpsm    — the blend rejects' own register shapes
static std::map<uint32_t, uint64_t> g_g603BlendShapes;
static uint64_t g_g603Fallbacks = 0ull;

static const char *g603ClsName(uint32_t cls)
{
    switch (cls)
    {
    case kG528ClsDisplay: return "disp";
    case kG528ClsLegacy: return "legacy";
    case kG528ClsDiscovered: return "discov";
    default: return "other";
    }
}

static void g603Report()
{
    std::fprintf(stderr, "[G603:admit] fallbacks=%llu rows=%zu blendShapes=%zu\n",
                 static_cast<unsigned long long>(g_g603Fallbacks), g_g603Rows.size(),
                 g_g603BlendShapes.size());
    for (const auto &kv : g_g603Rows)
    {
        const uint32_t cls = kv.first >> 9;
        const uint32_t fbp = kv.first & 0x1FFu;
        const G603Row &r = kv.second;
        std::fprintf(stderr,
                     "[G603:row] cls=%-6s fbp=%03x fbw=%u batches=%llu entries=%llu "
                     "allOk=%llu allOkRelaxBlend=%llu allOkRelaxZ=%llu "
                     "allOkRelaxBoth=%llu shapeOk=%llu primMask=0x%llx "
                     "tme=%llu guestZ=%llu zWr=%llu zbp=%03x zpsm=%x\n",
                     g603ClsName(cls), fbp, r.fbwSeen,
                     static_cast<unsigned long long>(r.batches),
                     static_cast<unsigned long long>(r.entries),
                     static_cast<unsigned long long>(r.batchesAllOk),
                     static_cast<unsigned long long>(r.batchesAllOkRelax),
                     static_cast<unsigned long long>(r.batchesAllOkRelaxZ),
                     static_cast<unsigned long long>(r.batchesAllOkRelaxBoth),
                     static_cast<unsigned long long>(r.batchesShapeOk),
                     static_cast<unsigned long long>(r.primMask),
                     static_cast<unsigned long long>(r.texturedEntries),
                     static_cast<unsigned long long>(r.guestZEntries),
                     static_cast<unsigned long long>(r.zWriteEntries),
                     r.zbpSeen, r.zpsmSeen);
        std::fprintf(stderr, "[G603:site] cls=%-6s fbp=%03x shipped(", g603ClsName(cls), fbp);
        for (int i = 0; i < kG603SiteN; ++i)
            if (r.site[i])
                std::fprintf(stderr, "%s=%llu ", kG603SiteName[i],
                             static_cast<unsigned long long>(r.site[i]));
        std::fprintf(stderr, ") relaxBlend(");
        for (int i = 0; i < kG603SiteN; ++i)
            if (r.siteRelax[i])
                std::fprintf(stderr, "%s=%llu ", kG603SiteName[i],
                             static_cast<unsigned long long>(r.siteRelax[i]));
        std::fprintf(stderr, ")\n");
    }
    for (const auto &kv : g_g603BlendShapes)
    {
        const uint32_t abcd = kv.first >> 9;
        const uint32_t fix128 = (kv.first >> 8) & 1u;
        const uint32_t tpsm = kv.first & 0xFFu;
        std::fprintf(stderr,
                     "[G603:blend] A=%u B=%u C=%u D=%u fix128=%u tpsm=0x%02x entries=%llu\n",
                     abcd & 3u, (abcd >> 2) & 3u, (abcd >> 4) & 3u, (abcd >> 6) & 3u, fix128,
                     tpsm, static_cast<unsigned long long>(kv.second));
    }
    std::fflush(stderr);
}

// Run the SHIPPED classifier on one entry and return the site it rejected at (kG603SiteOk if it
// accepted). g262NoteReject stores the site unconditionally, so no duplicate predicate is needed
// here — a hand-written copy of the conjunct chain would drift from the real one within a phase.
static int g603ClassifySite(const G144Entry &e)
{
    G178EntryState st{};
    g_g266ClassifySite = -1;
    if (g178ClassifyEntry(e, st))
        return kG603SiteOk;
    const int site = g_g266ClassifySite;
    return (site >= 0 && site < kG262RejSiteCount) ? site : kG262RejFrame;
}

// The single hook. Called from the flush closure's CPU-fallback branch, immediately after
// g583NoteFallback, i.e. once per g178TryFlushGpu==false, for EVERY class of fallback.
static void g603NoteFallbackAdmission(const std::vector<G144Entry> &list)
{
    if (!g603AdmitCensusOn() || list.empty())
        return;
    const uint32_t fbp = list[0].ctx.frame.fbp & 0x1FFu;
    const uint32_t fbw = list[0].ctx.frame.fbw;
    const uint32_t cls = g528ClassifyFbp(list[0].ctx.frame.fbp);
    G603Row &r = g_g603Rows[(cls << 9) | fbp];
    ++r.batches;
    r.entries += list.size();
    r.fbwSeen = fbw;

    // The G586/G587 transient-target shape contract, evaluated for THIS batch: one target, one
    // width, CT32 throughout, and only the three primitive classes those phases admitted.
    bool shapeOk = list[0].ctx.frame.psm == GS_PSM_CT32;
    bool allOk = true, allOkRelax = true, allOkRelaxZ = true, allOkRelaxBoth = true;
    for (const G144Entry &e : list)
    {
        const GSContext &ctx = e.ctx;
        const uint32_t prim = static_cast<uint32_t>(e.prim.type);
        if (prim < 64u)
            r.primMask |= 1ull << prim;
        if (ctx.frame.fbp != list[0].ctx.frame.fbp || ctx.frame.fbw != fbw ||
            ctx.frame.psm != GS_PSM_CT32 ||
            !(prim == GS_PRIM_SPRITE || prim == GS_PRIM_TRIANGLE || prim == GS_PRIM_TRISTRIP))
            shapeOk = false;
        r.texturedEntries += e.prim.tme ? 1u : 0u;
        const uint64_t ts = ctx.test;
        const uint32_t zte = static_cast<uint32_t>((ts >> 16) & 1u);
        const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
        const bool zmsk = ((ctx.zbuf >> 32) & 1ull) != 0ull;
        if (zte != 0u && ztst != 1u)
        {
            ++r.guestZEntries;
            r.zbpSeen = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
            r.zpsmSeen = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
        }
        if (zte != 0u && !zmsk && ztst != 0u)
            ++r.zWriteEntries;

        const int site = g603ClassifySite(e);
        ++r.site[site];
        if (site != kG603SiteOk)
            allOk = false;
        if (site == kG262RejBlend)
        {
            const uint64_t al = ctx.alpha;
            const uint32_t abcd = static_cast<uint32_t>((al & 3u) | (((al >> 2) & 3u) << 2) |
                                                        (((al >> 4) & 3u) << 4) |
                                                        (((al >> 6) & 3u) << 6));
            const uint32_t fix128 = (((al >> 32) & 0xFFull) == 128ull) ? 1u : 0u;
            const uint32_t tpsm = e.prim.tme ? (ctx.tex0.psm & 0xFFu) : 0xFFu;
            ++g_g603BlendShapes[(abcd << 9) | (fix128 << 8) | tpsm];
        }

        // pass 2 — the same entry with ONLY the blend reject relaxed, so "blend is the last
        // blocker" is a measurement instead of an inference.
        t_g603RelaxBlend = true;
        const int siteRelax = g603ClassifySite(e);
        t_g603RelaxBlend = false;
        ++r.siteRelax[siteRelax];
        if (siteRelax != kG603SiteOk)
            allOkRelax = false;

        // G604 pass 3 — DEPTH relaxed only. This is the `discov` population's question: those
        // entries stop at `depth` because a G527-discovered target holds no guest-depth contract,
        // and the roadmap's Target 1 is to generalize G570/G586/G587's transient contract to a
        // PROPERTY. What this pass reports is what that lever would face NEXT.
        t_g604RelaxDepth = true;
        const int siteRelaxZ = g603ClassifySite(e);
        ++r.siteRelaxZ[siteRelaxZ];
        if (siteRelaxZ != kG603SiteOk)
            allOkRelaxZ = false;
        // pass 4 — depth AND blend, i.e. the ceiling of both levers together.
        t_g603RelaxBlend = true;
        const int siteRelaxBoth = g603ClassifySite(e);
        t_g603RelaxBlend = false;
        t_g604RelaxDepth = false;
        ++r.siteRelaxBoth[siteRelaxBoth];
        if (siteRelaxBoth != kG603SiteOk)
            allOkRelaxBoth = false;
    }
    if (shapeOk)
        ++r.batchesShapeOk;
    if (allOk)
        ++r.batchesAllOk;
    if (allOkRelax)
        ++r.batchesAllOkRelax;
    if (allOkRelaxZ)
        ++r.batchesAllOkRelaxZ;
    if (allOkRelaxBoth)
        ++r.batchesAllOkRelaxBoth;
    if ((++g_g603Fallbacks % 2048ull) == 0ull)
        g603Report();
}

} // namespace
