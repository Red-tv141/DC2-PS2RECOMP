// G343 arc#4 slice-1 premise-gate: per-shape TIME census of the local→local transfer edge — the
// downstream consumer floor that pins the 0x139 family to guest VRAM (G342: retiring any view-able
// c5 consumer relocates its readback row-exact to the next demander; the l2l copy at the
// g144FlushPendingLocalTransferRange edge is the first non-view-able demander). Behavior-pure,
// default-off (DC2_G343_CENSUS=1). Answers the arc#4 question no existing census does: which l2l
// COPY SHAPES (src tuple → dst tuple) force the c3 materialize, ranked by TIME, how many resident
// rows each publishes (src-side read demand vs dst-side write demand), and which are servable by a
// GPU-side copy (G280 CT32 64x32 page-tile model / G289 display-grab template / G285 page-consume
// geometry) vs genuinely cross-format. Complements: DC2_G327_CENSUS (upload edge, already has
// noteNs/matNs), DC2_G342_CENSUS (c5 tex-alias consumers), DC2_G340_CENSUS (c3 consumed-ness),
// DC2_G279_PROFILE (per-cause totals). Single flush-owner thread contract (as g_g266TexShapes).
static bool g343CensusOn() { static const bool v = envFlagEnabled("DC2_G343_CENSUS"); return v; }

// G343 slice-A BEHAVIOR lever (default-off): generalize the G285 destructive l2l page consume to
// admit a copy dst that begins at ANY 8KB-page-aligned offset at/after the dirty span start (not
// only strictly inside the FIRST dirty page row). Census evidence (MAP-0, [G343:shape]): the two
// dominant c3 shapes are the per-frame display grabs (0xd00/0x0 → 0x2760, 512x416 CT32), whose dst
// begins exactly ONE page-row past 0x139's dirty start — G285's `dstBase >= dirtyStart +
// pageRowBytes` boundary rejects them, so each edge publishes the atlas's full 416-row dirty
// window that the copy then overwrites bar the first 32 rows. With the lever on, the uncovered
// prefix page-rows are published and the covered suffix is consumed (identical post-state to the
// proven G285 path: prefix materialized, snapshot invalidated, caller's copy replaces the rest).
// G346: the closure arm implies the exact suffix-consume admission (stack member, arc-total-closure §3).
static bool g343L2lExactOn() { static const bool v = envFlagEnabled("DC2_G343_L2L_EXACT"); return v || g345ClosureOn(); }
static std::atomic<uint64_t> g_g343ConsumeAdmits{0};

struct G343Bucket
{
    uint64_t matNs = 0;  // g289-owner + depth-flush + g285-consume + c3 materialize span
    uint64_t pubNs = 0;  // display-owner depth + g272 + g277 + g276 publication span
    uint64_t srcRows = 0; // dirty resident rows the copy's SOURCE read demanded (pre-scan)
    uint64_t dstRows = 0; // dirty resident rows the copy's DEST write demanded (pre-scan)
    uint64_t fcRows = 0;  // dst-only rows whose whole dirty byte span the copy dst FULLY covers
    uint64_t n = 0;
    uint64_t nUnknown = 0; // edges with unknown src/dst range (fail-closed all-dirty)
    uint32_t hitFbp = 0;   // fbp of the last dirty resident target this shape materialized
};
static std::map<std::pair<uint64_t, uint64_t>, G343Bucket> g_g343Shapes;
static uint64_t g_g343TotalMatNs = 0, g_g343TotalPubNs = 0, g_g343TotalN = 0;

// Replicates the g261MaterializeForRanges dirty-overlap decision (read-only) so each shape can be
// attributed the resident rows its src read vs dst write would force. Must stay in lockstep with
// that function's overlap logic.
static void g343EstimateRows(const G144BlockRange &src, const G144BlockRange &dst,
                             bool srcUnknown, bool dstUnknown,
                             uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                             uint32_t rrw, uint32_t rrh,
                             uint64_t &srcRows, uint64_t &dstRows, uint64_t &fcRows,
                             uint32_t &unknown, uint32_t &hitFbp)
{
    srcRows = 0;
    dstRows = 0;
    fcRows = 0;
    unknown = (srcUnknown || dstUnknown) ? 1u : 0u;
    hitFbp = 0;
    if (!g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty || r.dirtyHi < r.dirtyLo)
            continue;
        const uint64_t rows = static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
        if (unknown || !r.range.valid)
        {
            srcRows += rows; // fail-closed: attributed as source demand
            hitFbp = kG261Fbp[i];
            continue;
        }
        const bool sHit = src.valid && g144RangeOverlaps(r.range, src);
        const bool dHit = dst.valid && g144RangeOverlaps(r.range, dst);
        if (sHit)
        {
            srcRows += rows;
            hitFbp = kG261Fbp[i];
        }
        else if (dHit)
        {
            dstRows += rows;
            hitFbp = kG261Fbp[i];
            // Full-cover test (g285 byte geometry, but allowing dstBase == dirtyStart): does the
            // CT32 copy dst byte span cover the target's ENTIRE dirty page-row span? If yes, the
            // materialize publishes bytes the copy immediately overwrites in full — the exact
            // destructive-consume class slice-A would elide.
            if (dpsm == GS_PSM_CT32 && r.fbw == dbw && dbw != 0u &&
                (r.dirtyLo & 31) == 0 && ((r.dirtyHi + 1) & 31) == 0 &&
                rrw == static_cast<uint32_t>(dbw) * 64u)
            {
                constexpr uint64_t kPageBytes = 8192ull;
                const uint64_t targetBase =
                    static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[i])) * 256ull;
                const uint64_t pageRowBytes = static_cast<uint64_t>(r.fbw) * kPageBytes;
                const uint64_t dirtyStart =
                    targetBase + static_cast<uint64_t>(r.dirtyLo / 32) * pageRowBytes;
                const uint64_t dirtyEnd =
                    targetBase + static_cast<uint64_t>((r.dirtyHi + 1) / 32) * pageRowBytes;
                const uint64_t dstBase = static_cast<uint64_t>(dbp) * 256ull;
                const uint64_t dstEnd =
                    dstBase + static_cast<uint64_t>(rrw) * rrh * 4ull;
                if (dstBase <= dirtyStart && dstEnd >= dirtyEnd)
                    fcRows += rows;
            }
        }
    }
}

// GPU-side-copy feasibility class for arc#4, from copy geometry alone (no live state reads):
//  g289-disp  — the exact G289 display-grab shape (already GPU-served when its pend state holds).
//  g285-page  — passes G285's full-width page-aligned CT32 same-bw consume geometry.
//  ct32-same  — CT32→CT32 same bw: G280 64x32 page-tile copy model applies directly.
//  ct32-bwdiff— CT32→CT32 differing bw: page tiles are layout-independent, needs remap plan.
//  xfmt       — differing/non-CT32 PSM: no proven within-page order equivalence (G280 rule).
static const char *g343Class(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                             uint32_t ssax, uint32_t ssay,
                             uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                             uint32_t dsax, uint32_t dsay,
                             uint32_t rrw, uint32_t rrh)
{
    if (spsm != GS_PSM_CT32 || dpsm != GS_PSM_CT32)
        return "xfmt";
    if (sbw == 8u && dbw == 8u && ssax == 0u && ssay == 0u && dsax == 0u && dsay == 0u &&
        rrw == 512u && rrh == 416u &&
        (dbp == framePageBaseToBlock(0x13bu) || dbp == framePageBaseToBlock(0x14au)))
        return "g289-disp";
    if (sbw == dbw && sbw != 0u && (sbp & 31u) == 0u && (dbp & 31u) == 0u &&
        ssax == 0u && ssay == 0u && dsax == 0u && dsay == 0u &&
        rrw == static_cast<uint32_t>(dbw) * 64u && rrh != 0u && (rrh & 31u) == 0u)
        return "g285-page";
    if (sbw == dbw)
        return "ct32-same";
    return "ct32-bwdiff";
}

static void g343Report();

static void g343NoteL2l(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                        uint32_t ssax, uint32_t ssay,
                        uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                        uint32_t dsax, uint32_t dsay,
                        uint32_t rrw, uint32_t rrh,
                        uint64_t srcRows, uint64_t dstRows, uint64_t fcRows,
                        uint32_t unknown, uint32_t hitFbp,
                        uint64_t matNs, uint64_t pubNs)
{
    uint64_t k1 = 0, k2 = 0;
    k1 |= static_cast<uint64_t>(sbp & 0x3FFFu);
    k1 |= static_cast<uint64_t>(dbp & 0x3FFFu) << 14;
    k1 |= static_cast<uint64_t>(spsm & 0x3Fu) << 28;
    k1 |= static_cast<uint64_t>(dpsm & 0x3Fu) << 34;
    k1 |= static_cast<uint64_t>(sbw & 0x3Fu) << 40;
    k1 |= static_cast<uint64_t>(dbw & 0x3Fu) << 46;
    k2 |= static_cast<uint64_t>(rrw & 0xFFFu);
    k2 |= static_cast<uint64_t>(rrh & 0xFFFu) << 12;
    k2 |= static_cast<uint64_t>(ssax & 0x7FFu) << 24;
    k2 |= static_cast<uint64_t>(ssay & 0x7FFu) << 35;
    k2 |= static_cast<uint64_t>(dsax & 0x7FFu) << 46;
    k2 |= static_cast<uint64_t>(dsay & 0x3Fu) << 57;
    G343Bucket &b = g_g343Shapes[{k1, k2}];
    b.matNs += matNs;
    b.pubNs += pubNs;
    b.srcRows += srcRows;
    b.dstRows += dstRows;
    b.fcRows += fcRows;
    b.n += 1u;
    b.nUnknown += unknown;
    if (hitFbp != 0u)
        b.hitFbp = hitFbp;
    g_g343TotalMatNs += matNs;
    g_g343TotalPubNs += pubNs;
    g_g343TotalN += 1u;
    if ((g_g343TotalN % 512u) == 0u)
        g343Report();
}

static void g343Report()
{
    if (!g343CensusOn())
        return;
    std::fprintf(stderr,
                 "[G343:l2lcensus] shapes=%zu totalN=%llu matMs=%.1f pubMs=%.1f "
                 "(l2l edge by copy shape, ranked by mat time)\n",
                 g_g343Shapes.size(), static_cast<unsigned long long>(g_g343TotalN),
                 g_g343TotalMatNs / 1e6, g_g343TotalPubNs / 1e6);
    std::vector<const std::pair<const std::pair<uint64_t, uint64_t>, G343Bucket> *> order;
    order.reserve(g_g343Shapes.size());
    for (const auto &kv : g_g343Shapes)
        order.push_back(&kv);
    std::sort(order.begin(), order.end(),
              [](const std::pair<const std::pair<uint64_t, uint64_t>, G343Bucket> *a,
                 const std::pair<const std::pair<uint64_t, uint64_t>, G343Bucket> *b) {
                  return a->second.matNs > b->second.matNs;
              });
    for (const auto *p : order)
    {
        const uint64_t k1 = p->first.first;
        const uint64_t k2 = p->first.second;
        const G343Bucket &b = p->second;
        const uint32_t sbp = static_cast<uint32_t>(k1 & 0x3FFFu);
        const uint32_t dbp = static_cast<uint32_t>((k1 >> 14) & 0x3FFFu);
        const uint8_t spsm = static_cast<uint8_t>((k1 >> 28) & 0x3Fu);
        const uint8_t dpsm = static_cast<uint8_t>((k1 >> 34) & 0x3Fu);
        const uint8_t sbw = static_cast<uint8_t>((k1 >> 40) & 0x3Fu);
        const uint8_t dbw = static_cast<uint8_t>((k1 >> 46) & 0x3Fu);
        const uint32_t rrw = static_cast<uint32_t>(k2 & 0xFFFu);
        const uint32_t rrh = static_cast<uint32_t>((k2 >> 12) & 0xFFFu);
        const uint32_t ssax = static_cast<uint32_t>((k2 >> 24) & 0x7FFu);
        const uint32_t ssay = static_cast<uint32_t>((k2 >> 35) & 0x7FFu);
        const uint32_t dsax = static_cast<uint32_t>((k2 >> 46) & 0x7FFu);
        const uint32_t dsay = static_cast<uint32_t>((k2 >> 57) & 0x3Fu);
        std::fprintf(stderr,
                     "[G343:shape] class=%-10s matMs=%8.1f pubMs=%7.1f srcRows=%9llu "
                     "dstRows=%9llu fcRows=%9llu hit=%03x n=%6llu unk=%llu | "
                     "src=0x%x/%u/0x%02x@%u,%u -> dst=0x%x/%u/0x%02x@%u,%u %ux%u\n",
                     g343Class(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay,
                               rrw, rrh),
                     b.matNs / 1e6, b.pubNs / 1e6,
                     static_cast<unsigned long long>(b.srcRows),
                     static_cast<unsigned long long>(b.dstRows),
                     static_cast<unsigned long long>(b.fcRows), b.hitFbp,
                     static_cast<unsigned long long>(b.n),
                     static_cast<unsigned long long>(b.nUnknown),
                     sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                     dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh);
    }
}
