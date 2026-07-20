// ===== G294 per-physical-page authoritative-owner token (default-off, DC2_G294_OWNER_TOKEN=1) =====
// G283's page epoch proves "someone wrote this page since resolve" but not WHO. With a REAL
// multi-owner page (G292: physical page 0x2860 aliased by 0x13c and 0x139 with divergent content),
// g280TryEntry copied EVERY dirty sibling overlapping the page into the same dst tile
// (last-writer-wins) and the exact verifier caught a full-page mismatch. G294 records the CURRENT
// authoritative owner target per physical CT32 page — the last target to INDEPENDENTLY write it
// (wave render or VRAM upload; a g280/g282 COPY does not restamp) — plus a monotonic equivalence
// version. g280 then copies a page ONLY from its authoritative owner; a non-owner (divergent)
// source is skipped and the dst keeps existing VRAM/overlay bytes (fail-closed). A g282 fanout
// leaves owner=source, so its byte-identical destinations share the owner's version implicitly; the
// next independent write to any of them re-stamps owner to that writer, staling the equivalents.
struct G294Owner
{
    int32_t ownerTi = -1;
    uint64_t equivVer = 0u;
};
static std::unordered_map<uint32_t, G294Owner> g_g294Owner; // pageBlock -> authoritative owner
static uint64_t g_g294EquivSeq = 0u; // monotonic version source (flush-owner thread only)
static std::atomic<uint64_t> g_g294Stamps{0}, g_g294OwnerSel{0}, g_g294NonOwnerSkip{0};
static bool g294OwnerTokenOn()
{
    // Rides the G280 alias-view (NOT G283): the owner-token REPLACES G282's write-side fanout as
    // the multi-owner arbiter. G283's fanout pre-equalizes aliases so multiOwner never diverges
    // (exact but neutral — it forces G291 skips=0, G292); the owner-token instead lets g280's
    // read-side copy pick the one authoritative source, leaving G291's upload-re-coverage retire
    // free to pay. commitOvl replaces the overlay per pageBlock (most-recent owner wins), so no
    // per-overlay equivalence version is needed for exactness.
    static const bool s_req = g294OwnerTokenRequested();
    return s_req && g280AliasOn();
}
static bool g294StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G294_STAT");
    return s_on;
}
// An INDEPENDENT write (render/upload) into target ti is the fresh authority for pageBlock.
static void g294StampOwner(int ti, uint32_t pageBlock)
{
    if (!g294OwnerTokenOn())
        return;
    G294Owner &o = g_g294Owner[pageBlock];
    o.ownerTi = ti;
    o.equivVer = ++g_g294EquivSeq;
    g_g294Stamps.fetch_add(1u, std::memory_order_relaxed);
}
// Current authoritative owner target of pageBlock, or -1 if none/authority-off.
static int g294PageOwner(uint32_t pageBlock)
{
    if (!g294OwnerTokenOn())
        return -1;
    auto it = g_g294Owner.find(pageBlock);
    return it == g_g294Owner.end() ? -1 : it->second.ownerTi;
}

static bool g309On()
{
    // Publication precedence comes directly from the resident target set. G294's last-writer
    // token describes a different semantic and is neither required nor implied.
    return g309Requested() && g261WaveOn();
}

static bool g309VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G309_VERIFY");
    return s_on && g309On();
}

static bool g309StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G309_STAT");
    return s_on;
}

static void g309Report()
{
    if (!g309On() && !g309StatOn() &&
        g_g309Composites.load(std::memory_order_relaxed) == 0u)
        return;
    std::fprintf(stderr,
                 "[G309:stat] cand=%llu composites=%llu binds=%llu "
                 "fail(missing=%llu layout=%llu backend=%llu stale=%llu) "
                 "verify(n=%llu pixels=%llu bad=%llu)\n",
                 (unsigned long long)g_g309Candidates.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309Composites.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309Binds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309FailMissing.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309FailLayout.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309FailBackend.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309FailStale.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309VerifyComposites.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309VerifyPixels.load(std::memory_order_relaxed),
                 (unsigned long long)g_g309VerifyBad.load(std::memory_order_relaxed));
}

static bool g310Requested()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G310_NO_LOGICAL") ||
            envFlagDisabled("DC2_G310_LOGICAL"))
            return false;
        return true;
    }();
    return s_on;
}

static bool g310On()
{
    // The logical surface replaces the default five-target publication path. It deliberately
    // does not combine with the older overlay/fan-out experiment, whose copied FBO pages have a
    // separate ownership model. A user forcing both gets the older arm and G310 stays inert.
    return g310Requested() && g261WaveOn() && !g280AliasOn();
}

static bool g310VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G310_VERIFY");
    return s_on && g310On();
}

static bool g310StatOn()
{
    static const bool s_on =
        envFlagEnabled("DC2_G310_STAT") ||
        envFlagEnabled("DC2_G310_CENSUS") ||
        envFlagEnabled("DC2_G310_VERIFY");
    return s_on;
}

static bool g310AliasRetireOn()
{
    // Diagnostic kill isolates the logical display resource from its required downstream
    // producer repair. The master G310 kill still disables the complete arm.
    static const bool s_enabled =
        !envFlagEnabled("DC2_G310_NO_ALIAS_RETIRE") &&
        !envFlagDisabled("DC2_G310_ALIAS_RETIRE");
    return s_enabled && g310On() && !g310VerifyOn();
}

static bool g310T8ViewOn()
{
    // The measured atlas T8 reads are a consumer of the same logical/physical split, but they
    // need a decoded snapshot because OpenGL cannot safely sample and render the 0x139 FBO at
    // once. G281 already supplies that exact byte-lane decoder. Keep a narrow diagnostic kill
    // and leave G310's legacy-comparison verifier behavior-pure.
    static const bool s_enabled =
        !envFlagEnabled("DC2_G310_NO_T8_VIEW") &&
        !envFlagDisabled("DC2_G310_T8_VIEW");
    return s_enabled && g310On() && !g310VerifyOn();
}

// G311 incremental page-delta refresh. Default-off during bring-up (opt-in DC2_G311_INCREMENTAL);
// after promotion this flips to default-on with kill DC2_G311_NO_INCREMENTAL. The full G310
// composite remains the immutable control arm and the fail-closed recovery/oracle path.
static bool g311On()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G311_NO_INCREMENTAL") ||
            envFlagDisabled("DC2_G311_INCREMENTAL"))
            return false;
        // Bring-up default: require the explicit opt-in. Flip to `return true;` at promotion.
        return envFlagEnabled("DC2_G311_INCREMENTAL");
    }();
    // The G310 verifier compares the full-composite snapshot to legacy; keep the incremental path
    // out of that behavior-pure oracle so DC2_G310_VERIFY always exercises the control arm.
    return s_on && g310On() && !g310VerifyOn();
}

static bool g311CensusOn()
{
    // Premise-gate instrument: times the full composite + histograms changed pages per rebuild.
    // Zero behavior change; safe to leave gated with DC2_G310_STAT for a combined stat run.
    static const bool s_on =
        envFlagEnabled("DC2_G311_CENSUS") ||
        envFlagEnabled("DC2_G311_INCREMENTAL") ||
        envFlagEnabled("DC2_G311_VERIFY");
    return s_on;
}

static bool g311VerifyOn()
{
    // Self-contained exact oracle: after an incremental refresh, compare the logical FBO readback
    // to a freshly recomputed full G309 composite of the same desired page map (bad==0 proves
    // incremental == full-composite; G310 already proved full-composite == legacy publication).
    static const bool s_on = envFlagEnabled("DC2_G311_VERIFY");
    return s_on && g311On();
}

static void g310Report()
{
    if (!g310StatOn() && !g310On() &&
        g_g310Prepares.load(std::memory_order_relaxed) == 0u)
        return;
    std::fprintf(
        stderr,
        "[G310:stat] on=%d init=%d frozen=%d cand=%llu prep=%llu binds=%llu "
        "initN=%llu refresh=%llu invalid=%llu composite=%llu "
        "gpu(j=%llu p=%llu) guest(j=%llu p=%llu) "
        "retire(b=%llu t=%llu p=%llu fail=%llu) "
        "noop=%llu authorityMoves=%llu fail(layout=%llu backend=%llu missing=%llu) "
        "verify(n=%llu pixels=%llu bad=%llu)\n",
        g310On() ? 1 : 0, g_g310Initialized ? 1 : 0, g_g310Frozen ? 1 : 0,
        (unsigned long long)g_g310Candidates.load(std::memory_order_relaxed),
        (unsigned long long)g_g310Prepares.load(std::memory_order_relaxed),
        (unsigned long long)g_g310Binds.load(std::memory_order_relaxed),
        (unsigned long long)g_g310Init.load(std::memory_order_relaxed),
        (unsigned long long)g_g310Refresh.load(std::memory_order_relaxed),
        (unsigned long long)g_g310Invalidations.load(std::memory_order_relaxed),
        (unsigned long long)g_g310CompositeJobs.load(std::memory_order_relaxed),
        (unsigned long long)g_g310GpuJobs.load(std::memory_order_relaxed),
        (unsigned long long)g_g310GpuPages.load(std::memory_order_relaxed),
        (unsigned long long)g_g310GuestJobs.load(std::memory_order_relaxed),
        (unsigned long long)g_g310GuestPages.load(std::memory_order_relaxed),
        (unsigned long long)g_g310RetireBatches.load(std::memory_order_relaxed),
        (unsigned long long)g_g310RetireTargets.load(std::memory_order_relaxed),
        (unsigned long long)g_g310RetirePages.load(std::memory_order_relaxed),
        (unsigned long long)g_g310RetireFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g310NoopPages.load(std::memory_order_relaxed),
        (unsigned long long)g_g310AuthorityMoves.load(std::memory_order_relaxed),
        (unsigned long long)g_g310FailLayout.load(std::memory_order_relaxed),
        (unsigned long long)g_g310FailBackend.load(std::memory_order_relaxed),
        (unsigned long long)g_g310FailMissing.load(std::memory_order_relaxed),
        (unsigned long long)g_g310VerifyN.load(std::memory_order_relaxed),
        (unsigned long long)g_g310VerifyPixels.load(std::memory_order_relaxed),
        (unsigned long long)g_g310VerifyBad.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G310:producer]");
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        std::fprintf(
            stderr, " %03x=%llu/%llup", kG261Fbp[t],
            (unsigned long long)g_g310ProducerN[t].load(std::memory_order_relaxed),
            (unsigned long long)g_g310ProducerPages[t].load(std::memory_order_relaxed));
    std::fprintf(stderr, " sources");
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        std::fprintf(
            stderr, " %03x=%llu", kG261Fbp[t],
            (unsigned long long)g_g310SourcePages[t].load(std::memory_order_relaxed));
    std::fprintf(
        stderr, " guest=%llu\n[G310:mat]",
        (unsigned long long)g_g310SourcePages[kG248TargetCount].load(
            std::memory_order_relaxed));
    for (int cause = 0; cause < 8; ++cause)
        std::fprintf(
            stderr, " c%d=%llu/%llup", cause,
            (unsigned long long)g_g310MatN[cause].load(std::memory_order_relaxed),
            (unsigned long long)g_g310MatPages[cause].load(std::memory_order_relaxed));
    std::fprintf(
        stderr,
        "\n[G310:present] reads=%llu overlap=%llu pages=%llu unknown=%llu\n",
        (unsigned long long)g_g310PresentReads.load(std::memory_order_relaxed),
        (unsigned long long)g_g310PresentOverlap.load(std::memory_order_relaxed),
        (unsigned long long)g_g310PresentPages.load(std::memory_order_relaxed),
        (unsigned long long)g_g310PresentUnknown.load(std::memory_order_relaxed));
    std::fprintf(
        stderr,
        "[G310:t8] cand=%llu views=%llu buildFail=%llu rej(shape=%llu page=%llu clut=%llu)\n",
        (unsigned long long)g_g310T8Candidates.load(std::memory_order_relaxed),
        (unsigned long long)g_g310T8Views.load(std::memory_order_relaxed),
        (unsigned long long)g_g310T8BuildFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g310T8RejShape.load(std::memory_order_relaxed),
        (unsigned long long)g_g310T8RejPage.load(std::memory_order_relaxed),
        (unsigned long long)g_g310T8RejClut.load(std::memory_order_relaxed));
    g311Report();
}

static void g311Report()
{
    if (!g311CensusOn() && !g311On())
        return;
    const uint64_t compN = g_g311CompositeN.load(std::memory_order_relaxed);
    const uint64_t incN = g_g311IncrementalN.load(std::memory_order_relaxed);
    const uint64_t compUs = g_g311CompositeUs.load(std::memory_order_relaxed);
    const uint64_t incUs = g_g311IncrementalUs.load(std::memory_order_relaxed);
    std::fprintf(
        stderr,
        "[G311:stat] on=%d stale=%d refresh(n=%llu work=%llu noop=%llu) stales=%llu "
        "composite(n=%llu us=%llu avg=%.1f) incremental(n=%llu us=%llu avg=%.1f) "
        "verify(n=%llu pixels=%llu bad=%llu miss=%llu)\n",
        g311On() ? 1 : 0, g_g310Stale ? 1 : 0,
        (unsigned long long)g_g311Refreshes.load(std::memory_order_relaxed),
        (unsigned long long)g_g311RefreshWork.load(std::memory_order_relaxed),
        (unsigned long long)g_g311RefreshNoop.load(std::memory_order_relaxed),
        (unsigned long long)g_g311Stales.load(std::memory_order_relaxed),
        (unsigned long long)compN, (unsigned long long)compUs,
        compN ? static_cast<double>(compUs) / static_cast<double>(compN) : 0.0,
        (unsigned long long)incN, (unsigned long long)incUs,
        incN ? static_cast<double>(incUs) / static_cast<double>(incN) : 0.0,
        (unsigned long long)g_g311VerifyN.load(std::memory_order_relaxed),
        (unsigned long long)g_g311VerifyPixels.load(std::memory_order_relaxed),
        (unsigned long long)g_g311VerifyBad.load(std::memory_order_relaxed),
        (unsigned long long)g_g311VerifyMiss.load(std::memory_order_relaxed));
    // Changed-page histogram per full composite: the deletion ceiling for the incremental path.
    // Buckets: 0, 1-4, 5-16, 17-32, 33-64, 65-96, 97-127, 128 pages changed vs the prior snapshot.
    std::fprintf(stderr, "[G311:delta] hist(0=%llu 1-4=%llu 5-16=%llu 17-32=%llu "
                         "33-64=%llu 65-96=%llu 97-127=%llu 128=%llu)\n",
                 (unsigned long long)g_g311DeltaHist[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g311DeltaHist[7].load(std::memory_order_relaxed));
}