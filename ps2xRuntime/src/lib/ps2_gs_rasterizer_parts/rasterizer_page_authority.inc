// ===== G283 PHYSICAL-PAGE AUTHORITY / EPOCH (default-off, DC2_G283_AUTHORITY=1) =====
// G280/G281/G282 track per-TARGET contentGen, which cannot express that TWO different resident
// targets alias the SAME physical GS page (CT32 pages are shared VRAM addressed under different
// FRAME/FBW layouts). G282's write-side fan-out proved a real payoff (removed 205 CPU
// materializations at 3,072 waves) but was UNSAFE with more than one dirty sibling owning a page:
// an independent later write to one alias left the other alias's overlay/copy stale, and the exact
// G280 verifier caught a full mismatched page 0x2860 (0x13c vs 0x139). G283 adds a per-PHYSICAL-PAGE
// monotonic epoch bumped on EVERY write to ANY aliasing target; G280 overlays and G282 fan-out
// records store the epoch at resolve time and are treated as stale (re-resolved or VRAM-re-synced)
// the moment any peer alias writes the page. This is the arbitration that lets multi-owner fan-out
// hold exactly, so the measured G282 batch can keep the five targets resident without a stale peer.
// The mechanism is authority-off inert: g283NoteTargetPages/g283PageEpoch short-circuit and the
// fan-out keeps G282's single-owner fail-close.
static bool g283AuthorityRequested()
{
    if (envFlagEnabled("DC2_G283_NO_AUTHORITY") || envFlagDisabled("DC2_G283_AUTHORITY"))
        return false;
    return envFlagEnabled("DC2_G283_AUTHORITY");
}
static bool g283AuthorityOn()
{
    // G294 turns the epoch ON (cross-pass overlay staleness) WITHOUT g282's write-side fanout:
    // the owner-token gives in-pass source selection, the epoch gives cross-pass invalidation, and
    // skipping the fanout (which pre-equalizes aliases and forces G291 skips=0) leaves the retire
    // free to pay. g282PrepareFanout still only runs under g282TightRowsOn (false under g294).
    static const bool s_req = g283AuthorityRequested() || g294OwnerTokenRequested();
    return s_req && (g282TightRowsOn() || g294OwnerTokenRequested());
}
static bool g283StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G283_STAT");
    return s_on;
}
// pageBlock (32-block-aligned) -> monotonic epoch. Absent == 0. Flush-owner thread only (same
// contract as g_g280Ovl / g_g280ContentGen). At most 512 CT32 pages exist below 0x4000, so the
// map is tiny and never crosses a frame boundary in an unbounded way.
static std::unordered_map<uint32_t, uint64_t> g_g283Epoch;
static std::atomic<uint64_t> g_g283Bumps{0}, g_g283Stale{0}, g_g283MultiOwner{0},
    g_g283FanoutPages{0};
static uint64_t g283PageEpoch(uint32_t pageBlock)
{
    if (!g283AuthorityOn())
        return 0u;
    auto it = g_g283Epoch.find(pageBlock);
    return it == g_g283Epoch.end() ? 0u : it->second;
}
static uint64_t g283BumpPageEpoch(uint32_t pageBlock)
{
    uint64_t &e = g_g283Epoch[pageBlock];
    ++e;
    g_g283Bumps.fetch_add(1u, std::memory_order_relaxed);
    return e;
}