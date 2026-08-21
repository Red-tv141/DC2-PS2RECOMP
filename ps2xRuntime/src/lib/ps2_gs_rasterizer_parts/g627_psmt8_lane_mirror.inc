// ===== G627: PSMT8-in-CT32 local-transfer destinations, without the destination-triggered
// =====        surface readback.
//
// PREMISE (G626 §2, `ridepod`). Surface materialization is the largest single architectural item on
// the route's pole — 3.68 ms/f, 2.88 of it blocking GPU readback — and `[G340:ceiling]` says
// **99.2% of the published rows are `overwrite`**: the l2l copy's DESTINATION overlaps the resident
// target's dirty rows, so the FBO is read back into guest VRAM only for the copy to write over it.
// `[G343:shape]` names the dominant edge exactly once:
//
//     class=xfmt matMs=110.3 srcRows=0 dstRows=31360 hit=139 n=251
//       | src=0x3520/2/0x13@0,211 -> dst=0x3420/2/0x13@0,53  128x45
//
// PSMT8 -> PSMT8, bw=2, 128x45, both endpoints inside target 0x139's CT32 footprint, and
// **`srcRows=0`: 100% of the publication is triggered by the DESTINATION.**
//
// THE BLOCKER G626 RECORDED. A CT32 word is 4 bytes = 4 PSMT8 texels, and the copy's y range is not
// 4-aligned (`dsay=53`, `rrh=45`), so the boundary CT32 words are only PARTIALLY written. Rebuilding
// such a word from guest VRAM alone is impossible while the FBO is the authority for the untouched
// lanes — hence "blocked on PSMT8 sub-word packing".
//
// MECHANISM. The packing is only unsolvable on the CPU. On the GPU both halves are present at once:
// the copy's own bytes (which the CPU pixel body has just written to guest VRAM from an
// authoritative source) and the untouched lanes (which the FBO already holds). So instead of
// publishing the destination, this file
//   1. proves the copy's SOURCE bytes are VRAM-authoritative and its DESTINATION lands in a dirty
//      resident CT32 target,
//   2. lets the unchanged CPU pixel body run (it is 5,760 bytes; it was never the cost),
//   3. maps every destination PSMT8 texel to its (target, tx, ty, byte-lane) through the SHIPPED
//      address functions (G594's law: never hand-derive a swizzle), and
//   4. patches those bytes VRAM->FBO with a **per-byte-lane masked** write, so lanes the copy did
//      not touch keep the FBO's value bit for bit.
//
// That is G264's upload mirror with a lane mask, and it reuses G264's residency contract verbatim
// (`mirPending` scheduling, materialize-before-readback ordering, loud bounded failure that drops
// residency WITHOUT a readback).
//
// This file ships in two stages, both default-off until the gate runs:
//   DC2_G627_CENSUS=1   behaviour-pure census: which l2l edges are this class, how many CT32 words
//                       they touch, how many are FULLY covered vs partial, and whether the source is
//                       VRAM-authoritative. The coverage denominator for everything below.
//   DC2_G627_LANEMIR=1  the lever.            Rollback once promoted: DC2_G627_NO_LANEMIR=1.
//   DC2_G627_VERIFY=1   the exactness oracle (reads the patched rows back and compares every
//                       written lane against VRAM and every untouched lane against its pre-state).
//
// MUST follow rasterizer_vram_materialization.inc (g264InvPage, g_g261Res, g285PrepareL2lConsume)
// and precede rasterizer_draw_sprite.inc, which holds the only call site.

// ---- gates ------------------------------------------------------------------------------------
// Namespace-scope caches (G619's law: a function-local `static const bool` is a per-access MSVC
// `_Init_global_epoch` TLS guard, not a cache). These are read once per l2l edge (~4/frame), but the
// rule is cheap to honour and this file is new.
static const bool s_g627Census = envFlagEnabled("DC2_G627_CENSUS");
static const bool s_g627Verify = envFlagEnabled("DC2_G627_VERIFY");
static const bool s_g627Stat = envFlagEnabled("DC2_G627_STAT") || s_g627Verify;
// PROMOTION STATE: default-OFF until this phase's A B B A gate runs. `DC2_G627_LANEMIR=1` arms it,
// `DC2_G627_NO_LANEMIR=1` is the rollback that must keep working after promotion.
static constexpr bool kG627PromotedDefaultOn = false;
static const bool s_g627LaneMir = !envFlagEnabled("DC2_G627_NO_LANEMIR") &&
                                  (kG627PromotedDefaultOn || envFlagEnabled("DC2_G627_LANEMIR"));
static bool g627CensusOn() { return s_g627Census; }
static bool g627VerifyOn() { return s_g627Verify; }
static bool g627StatOn() { return s_g627Stat; }
static bool g627LaneMirOn() { return s_g627LaneMir; }

// ---- the destination plan ----------------------------------------------------------------------
// One entry per CT32 texel of one resident target that the PSMT8 copy destination touches, with the
// 4-bit mask of byte lanes it writes. Rows are kept sorted by ty then tx so the patch walk below can
// emit run-length rectangles the way g264FlushMirror does.
struct G627Texel
{
    uint16_t tx = 0u;
    uint16_t ty = 0u;
    uint8_t laneMask = 0u; // bit L set => byte lane L of this CT32 word is written by the copy
};

struct G627TargetPlan
{
    std::vector<G627Texel> texels;
    uint32_t lanes = 0u;   // total PSMT8 texels landed in this target
    uint32_t full = 0u;    // CT32 words with all four lanes written
    uint32_t part = 0u;    // CT32 words with 1..3 lanes written
    int tyLo = 512, tyHi = -1;
    bool oob = false;      // at least one destination texel fell outside this target's footprint
};

struct G627PlanKey
{
    uint32_t dbp = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint8_t dbw = 0u, dpsm = 0u;
    std::array<uint8_t, kG248TargetCount> targetFbw{};
    std::array<int16_t, kG248TargetCount> targetFbW{};
    bool operator==(const G627PlanKey &o) const
    {
        return dbp == o.dbp && dsax == o.dsax && dsay == o.dsay && rrw == o.rrw &&
               rrh == o.rrh && dbw == o.dbw && dpsm == o.dpsm &&
               targetFbw == o.targetFbw && targetFbW == o.targetFbW;
    }
};

struct G627Plan
{
    G627PlanKey key{};
    std::array<G627TargetPlan, kG248TargetCount> targets{};
    uint64_t stamp = 0u;
    bool valid = false;
};

static G627PlanKey g627MakeKey(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                               uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    G627PlanKey k{};
    k.dbp = dbp;
    k.dbw = dbw;
    k.dpsm = dpsm;
    k.dsax = dsax;
    k.dsay = dsay;
    k.rrw = rrw;
    k.rrh = rrh;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        k.targetFbw[t] = static_cast<uint8_t>(g_g261Res[t].fbw);
        k.targetFbW[t] = static_cast<int16_t>(g_g261Res[t].fbW);
    }
    return k;
}

// Build the (target, tx, ty, laneMask) plan for a PSMT8 destination rect. Uses the SHIPPED
// GSPSMT8::addrPSMT8 for the source-of-truth byte address and G264's probed CT32 page inverse for
// the word->(x,y) half, so no swizzle layout is hand-derived anywhere in this file.
static bool g627BuildPlan(const G627PlanKey &key, G627Plan &out)
{
    const G264InvPage &inv = g264InvPage();
    const uint64_t px64 = static_cast<uint64_t>(key.rrw) * key.rrh;
    if (!inv.valid || key.dpsm != GS_PSM_T8 || key.dbw == 0u ||
        px64 == 0u || px64 > 512ull * 512ull || (key.dbp & 31u) != 0u)
        return false;
    out = G627Plan{};
    out.key = key;

    // 512 rows x 512 texels of 4-bit lane masks, built as one pass over the destination rect and
    // then flattened. A whole-target scratch keeps the walk O(texels) instead of O(rect * target).
    static std::vector<uint8_t> s_mask; // single flush-owner thread (the G144 contract)
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const uint32_t fbw = key.targetFbw[t];
        const int fbW = key.targetFbW[t];
        if (fbw == 0u || fbW <= 0 || fbW > 512)
            continue;
        G627TargetPlan &tp = out.targets[t];
        const uint64_t targetBase =
            static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[t])) * 256ull;
        const uint64_t targetBytes = static_cast<uint64_t>(fbw) * 16ull * 8192ull;
        s_mask.assign(512u * 512u, 0u);
        bool any = false;
        for (uint32_t sy = 0u; sy < key.rrh; ++sy)
        {
            for (uint32_t sx = 0u; sx < key.rrw; ++sx)
            {
                const uint64_t off = GSPSMT8::addrPSMT8(key.dbp, key.dbw,
                                                        key.dsax + sx, key.dsay + sy);
                if (off < targetBase || off >= targetBase + targetBytes)
                {
                    tp.oob = true;
                    continue;
                }
                const uint64_t rel = off - targetBase;
                const uint32_t page = static_cast<uint32_t>(rel / 8192u);
                const uint32_t word = static_cast<uint32_t>((rel % 8192u) >> 2u);
                const uint32_t lane = static_cast<uint32_t>(rel & 3u);
                const uint32_t tx = (page % fbw) * 64u + inv.x[word];
                const uint32_t ty = (page / fbw) * 32u + inv.y[word];
                if (tx >= static_cast<uint32_t>(fbW) || ty >= 512u)
                {
                    tp.oob = true;
                    continue;
                }
                s_mask[ty * 512u + tx] |= static_cast<uint8_t>(1u << lane);
                ++tp.lanes;
                any = true;
            }
        }
        if (!any)
            continue;
        for (uint32_t ty = 0u; ty < 512u; ++ty)
        {
            bool rowAny = false;
            for (uint32_t tx = 0u; tx < static_cast<uint32_t>(fbW); ++tx)
            {
                const uint8_t m = s_mask[ty * 512u + tx];
                if (m == 0u)
                    continue;
                rowAny = true;
                tp.texels.push_back(G627Texel{static_cast<uint16_t>(tx),
                                              static_cast<uint16_t>(ty), m});
                if (m == 0xfu)
                    ++tp.full;
                else
                    ++tp.part;
            }
            if (rowAny)
            {
                if (tp.tyLo > static_cast<int>(ty)) tp.tyLo = static_cast<int>(ty);
                if (tp.tyHi < static_cast<int>(ty)) tp.tyHi = static_cast<int>(ty);
            }
        }
    }
    out.valid = true;
    return true;
}

// Small LRU keyed on the copy geometry AND every resident target's live layout, so a width flip
// invalidates it (the G326 rule). The dominant `ridepod` shape is a single key.
static G627Plan *g627GetPlan(const G627PlanKey &key)
{
    static std::vector<G627Plan> s_cache;
    static uint64_t s_stamp = 0u;
    for (G627Plan &p : s_cache)
        if (p.key == key)
        {
            p.stamp = ++s_stamp;
            return p.valid ? &p : nullptr;
        }
    try
    {
        G627Plan plan;
        if (!g627BuildPlan(key, plan))
        {
            plan = G627Plan{};
            plan.key = key;
            plan.valid = false;
        }
        plan.stamp = ++s_stamp;
        if (s_cache.size() < 16u)
        {
            s_cache.push_back(std::move(plan));
            return s_cache.back().valid ? &s_cache.back() : nullptr;
        }
        auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                       [](const G627Plan &a, const G627Plan &b) {
                                           return a.stamp < b.stamp;
                                       });
        *oldest = std::move(plan);
        return oldest->valid ? &*oldest : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}

// ---- the census ---------------------------------------------------------------------------------
// Keyed by copy shape exactly like [G343:shape] so the two censuses can be read side by side.
// Answers, for the population G626 named:
//   * how many l2l edges are PSMT8->PSMT8 into a DIRTY resident CT32 target (the admissible class),
//   * `words` touched and the FULL vs PARTIAL split (the sub-word packing question, measured),
//   * whether the copy's SOURCE reads rows the FBO owns (if it does, VRAM's source bytes are stale
//     and no amount of GPU-side destination work is exact -> fail closed),
//   * `pubRows`: what the destination-triggered materialize publishes today.
struct G627CensusBucket
{
    uint64_t n = 0u;
    uint64_t px = 0u;
    uint64_t words = 0u, full = 0u, part = 0u, lanes = 0u;
    uint64_t pubRows = 0u;      // rows the materialize would publish for this edge
    uint64_t dstInDirty = 0u;   // touched CT32 texels whose ty is inside dirtyLo..dirtyHi
    uint64_t dstOutDirty = 0u;  // ... outside it
    uint64_t srcDirtyHit = 0u;  // edges whose SOURCE byte span maps into this target's dirty rows
    uint64_t srcInTarget = 0u;  // edges whose SOURCE byte span maps into the target at all
    uint64_t oob = 0u;
    uint32_t fbp = 0u;
    int dirtyLo = 0, dirtyHi = -1, winLo = 0, winHi = -1;
};
static std::map<std::pair<uint64_t, uint64_t>, G627CensusBucket> g_g627Census;
static uint64_t g_g627CensusEdges = 0u, g_g627CensusT8 = 0u, g_g627CensusOther = 0u,
                g_g627CensusNoTarget = 0u, g_g627CensusReports = 0u;

static void g627CensusReport();

// Does `bytesLo..bytesHi` (inclusive byte span) intersect the CT32 rows `rowLo..rowHi` of target t?
// Conservative: any byte inside a page whose page-row range covers those rows counts.
static bool g627ByteSpanHitsRows(uint32_t t, uint64_t bytesLo, uint64_t bytesHi,
                                 int rowLo, int rowHi)
{
    const G261Res &r = g_g261Res[t];
    if (r.fbw == 0u || rowHi < rowLo)
        return false;
    const uint64_t targetBase =
        static_cast<uint64_t>(framePageBaseToBlock(kG261Fbp[t])) * 256ull;
    const uint64_t pageRowBytes = static_cast<uint64_t>(r.fbw) * 8192ull;
    const uint64_t lo = targetBase + static_cast<uint64_t>(rowLo / 32) * pageRowBytes;
    const uint64_t hi = targetBase +
                        static_cast<uint64_t>(rowHi / 32 + 1) * pageRowBytes; // exclusive
    return bytesLo < hi && bytesHi >= lo;
}

static void g627NoteCensus(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                           uint32_t ssax, uint32_t ssay,
                           uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                           uint32_t dsax, uint32_t dsay,
                           uint32_t rrw, uint32_t rrh,
                           const G144BlockRange &src, const G144BlockRange &dst,
                           bool srcUnknown, bool dstUnknown)
{
    if (!g627CensusOn() || !g261WaveOn() || rrw == 0u || rrh == 0u)
        return;
    ++g_g627CensusEdges;
    if (spsm != GS_PSM_T8 || dpsm != GS_PSM_T8 || sbw == 0u || dbw == 0u ||
        srcUnknown || dstUnknown || !src.valid || !dst.valid)
    {
        ++g_g627CensusOther;
        return;
    }
    ++g_g627CensusT8;

    // Which dirty resident targets does this destination actually reach?
    G627Plan *plan = g627GetPlan(g627MakeKey(dbp, dbw, dpsm, dsax, dsay, rrw, rrh));
    if (plan == nullptr)
    {
        ++g_g627CensusNoTarget;
        return;
    }

    // Byte span of the source rect, for the authority question. addrPSMT8 is page-periodic and
    // monotone in y for a page-aligned bp, so the first/last row bound the whole span.
    uint64_t srcLo = ~0ull, srcHi = 0u;
    for (uint32_t sy = 0u; sy < rrh; sy += (rrh > 2u ? rrh - 1u : 1u))
        for (uint32_t sx = 0u; sx < rrw; sx += (rrw > 2u ? rrw - 1u : 1u))
        {
            const uint64_t o = GSPSMT8::addrPSMT8(sbp, sbw, ssax + sx, ssay + sy);
            srcLo = std::min(srcLo, o);
            srcHi = std::max(srcHi, o);
        }

    bool anyTarget = false;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        const G627TargetPlan &tp = plan->targets[t];
        if (!r.dirty || tp.texels.empty())
            continue;
        anyTarget = true;
        uint64_t k1 = 0u, k2 = 0u;
        k1 |= static_cast<uint64_t>(sbp & 0x3FFFu);
        k1 |= static_cast<uint64_t>(dbp & 0x3FFFu) << 14;
        k1 |= static_cast<uint64_t>(sbw & 0x3Fu) << 28;
        k1 |= static_cast<uint64_t>(dbw & 0x3Fu) << 34;
        k1 |= static_cast<uint64_t>(t & 0xFu) << 40;
        k2 |= static_cast<uint64_t>(rrw & 0xFFFu);
        k2 |= static_cast<uint64_t>(rrh & 0xFFFu) << 12;
        k2 |= static_cast<uint64_t>(ssay & 0x7FFu) << 24;
        k2 |= static_cast<uint64_t>(dsay & 0x7FFu) << 35;
        G627CensusBucket &b = g_g627Census[{k1, k2}];
        ++b.n;
        b.px += static_cast<uint64_t>(rrw) * rrh;
        b.words += tp.texels.size();
        b.full += tp.full;
        b.part += tp.part;
        b.lanes += tp.lanes;
        b.oob += tp.oob ? 1u : 0u;
        b.fbp = kG261Fbp[t];
        b.dirtyLo = r.dirtyLo;
        b.dirtyHi = r.dirtyHi;
        b.winLo = r.winLo;
        b.winHi = r.winHi;
        if (r.dirtyHi >= r.dirtyLo)
            b.pubRows += static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
        for (const G627Texel &tx : tp.texels)
        {
            if (static_cast<int>(tx.ty) >= r.dirtyLo && static_cast<int>(tx.ty) <= r.dirtyHi)
                ++b.dstInDirty;
            else
                ++b.dstOutDirty;
        }
        if (srcLo != ~0ull)
        {
            if (g627ByteSpanHitsRows(t, srcLo, srcHi, r.winLo, r.winHi))
                ++b.srcInTarget;
            if (g627ByteSpanHitsRows(t, srcLo, srcHi, r.dirtyLo, r.dirtyHi))
                ++b.srcDirtyHit;
        }
    }
    if (!anyTarget)
        ++g_g627CensusNoTarget;
    if ((g_g627CensusEdges % 256u) == 0u)
        g627CensusReport();
}

static void g627CensusReport()
{
    if (!g627CensusOn())
        return;
    ++g_g627CensusReports;
    std::fprintf(stderr,
                 "[G627:l2l] edges=%llu t8=%llu other=%llu noDirtyTarget=%llu shapes=%zu\n",
                 static_cast<unsigned long long>(g_g627CensusEdges),
                 static_cast<unsigned long long>(g_g627CensusT8),
                 static_cast<unsigned long long>(g_g627CensusOther),
                 static_cast<unsigned long long>(g_g627CensusNoTarget),
                 g_g627Census.size());
    std::vector<const std::pair<const std::pair<uint64_t, uint64_t>, G627CensusBucket> *> order;
    order.reserve(g_g627Census.size());
    for (const auto &kv : g_g627Census)
        order.push_back(&kv);
    std::sort(order.begin(), order.end(),
              [](const auto *a, const auto *b) { return a->second.px > b->second.px; });
    for (const auto *p : order)
    {
        const uint64_t k1 = p->first.first;
        const uint64_t k2 = p->first.second;
        const G627CensusBucket &b = p->second;
        const uint32_t sbp = static_cast<uint32_t>(k1 & 0x3FFFu);
        const uint32_t dbp = static_cast<uint32_t>((k1 >> 14) & 0x3FFFu);
        const uint32_t sbw = static_cast<uint32_t>((k1 >> 28) & 0x3Fu);
        const uint32_t dbw = static_cast<uint32_t>((k1 >> 34) & 0x3Fu);
        const uint32_t rrw = static_cast<uint32_t>(k2 & 0xFFFu);
        const uint32_t rrh = static_cast<uint32_t>((k2 >> 12) & 0xFFFu);
        const uint32_t ssay = static_cast<uint32_t>((k2 >> 24) & 0x7FFu);
        const uint32_t dsay = static_cast<uint32_t>((k2 >> 35) & 0x7FFu);
        const double fullPct = b.words ? 100.0 * static_cast<double>(b.full) /
                                             static_cast<double>(b.words)
                                       : 0.0;
        std::fprintf(stderr,
                     "[G627:shape] t=%03x n=%llu px=%llu words=%llu full=%llu(%.1f%%) part=%llu "
                     "lanes=%llu pubRows=%llu inDirty=%llu outDirty=%llu srcInWin=%llu "
                     "srcDirty=%llu oob=%llu dirty=%d..%d win=%d..%d | "
                     "src=0x%x/%u@y%u -> dst=0x%x/%u@y%u %ux%u\n",
                     b.fbp,
                     static_cast<unsigned long long>(b.n),
                     static_cast<unsigned long long>(b.px),
                     static_cast<unsigned long long>(b.words),
                     static_cast<unsigned long long>(b.full), fullPct,
                     static_cast<unsigned long long>(b.part),
                     static_cast<unsigned long long>(b.lanes),
                     static_cast<unsigned long long>(b.pubRows),
                     static_cast<unsigned long long>(b.dstInDirty),
                     static_cast<unsigned long long>(b.dstOutDirty),
                     static_cast<unsigned long long>(b.srcInTarget),
                     static_cast<unsigned long long>(b.srcDirtyHit),
                     static_cast<unsigned long long>(b.oob),
                     b.dirtyLo, b.dirtyHi, b.winLo, b.winHi,
                     sbp, sbw, ssay, dbp, dbw, dsay, rrw, rrh);
    }
}

// ---- the lever ----------------------------------------------------------------------------------
// Admission is decided at the l2l dependency-pin edge (before anything clears r.dirty) and the patch
// is APPLIED after the caller's CPU pixel body has written guest VRAM, because the patch's source of
// truth is exactly those freshly written bytes.
//
// EXACTNESS ARGUMENT, conjunct by conjunct:
//   1. spsm == dpsm == PSMT8, page-aligned bases, known rect => every destination byte address is
//      the SHIPPED GSPSMT8::addrPSMT8 value, and the copy body that produced it is unchanged.
//   2. The plan maps each of those byte addresses to (target, tx, ty, lane) through G264's probed
//      CT32 page inverse, and `oob` must be 0 => no destination byte escapes the target.
//   3. The copy's SOURCE must not overlap ANY dirty resident target's claimed range => guest VRAM is
//      authoritative for every byte the copy reads, so the bytes it wrote are the true GS result.
//      ([G627:shape] measures this as srcInWin/srcDirty; the dominant ridepod population reads 0
//      for both, over 9,207 of 9,462 admissible edges.)
//   4. Every dirty target the destination range overlaps must be covered by the plan => no
//      publication is skipped for a surface this patch does not reconcile.
//   5. Lanes the copy did not write are not in the mask, so the masked draw leaves the FBO's byte
//      untouched. That is the sub-word packing G626 recorded as blocked: unsolvable from VRAM,
//      trivial with the surface in hand.
//   6. The patched pages' generation baseline is re-anchored exactly as G264's mirror does, so the
//      copy's own VRAM write cannot later be mistaken for an unreconciled guest write and upload
//      stale VRAM over the FBO.
// Anything not proven above falls back to the unchanged publication.

static std::atomic<uint64_t> g_g627Admits{0}, g_g627Patches{0}, g_g627PatchTexels{0},
    g_g627PatchLanes{0}, g_g627Fails{0}, g_g627RowsSaved{0};
static std::atomic<uint64_t> g_g627RejPsm{0}, g_g627RejGeom{0}, g_g627RejPlan{0},
    g_g627RejSrc{0}, g_g627RejCover{0}, g_g627RejTarget{0}, g_g627RejArm{0};
static std::atomic<uint64_t> g_g627VfyChecks{0}, g_g627VfyBad{0};

struct G627Pin
{
    bool armed = false;
    uint32_t mask = 0u;
    G627PlanKey key{};
};
static thread_local G627Pin t_g627Pin;
static void g627DisarmPin() { t_g627Pin.armed = false; }

// Byte span of a PSMT8 rect. addrPSMT8 is page-periodic and monotone in the page index for a
// page-aligned bp, so probing the rect's four corners bounds every byte it touches.
static bool g627T8ByteSpan(uint32_t bp, uint8_t bw, uint32_t sax, uint32_t say,
                           uint32_t rrw, uint32_t rrh, uint64_t &lo, uint64_t &hi)
{
    if (rrw == 0u || rrh == 0u || bw == 0u)
        return false;
    lo = ~0ull;
    hi = 0u;
    const uint32_t xs[2] = {sax, sax + rrw - 1u};
    const uint32_t ys[2] = {say, say + rrh - 1u};
    for (uint32_t yi = 0; yi < 2u; ++yi)
        for (uint32_t xi = 0; xi < 2u; ++xi)
        {
            const uint64_t o = GSPSMT8::addrPSMT8(bp, bw, xs[xi], ys[yi]);
            lo = std::min(lo, o);
            hi = std::max(hi, o);
        }
    return lo != ~0ull;
}

// Returns the mask of resident targets whose kG261MatLocal publication this edge takes over.
// Zero means "changed nothing"; the caller then runs its unchanged materialize.
static uint32_t g627TryTakeL2lDest(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                   uint32_t ssax, uint32_t ssay,
                                   uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                   uint32_t dsax, uint32_t dsay,
                                   uint32_t rrw, uint32_t rrh,
                                   const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown)
{
    t_g627Pin.armed = false;
    if (!g627LaneMirOn())
        return 0u;
    if (!g261WaveOn() || !g261AnyDirty() || g280AliasOn())
    {
        g_g627RejArm.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    if (spsm != GS_PSM_T8 || dpsm != GS_PSM_T8)
    {
        g_g627RejPsm.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    if (sbw == 0u || dbw == 0u || rrw == 0u || rrh == 0u ||
        static_cast<uint64_t>(rrw) * rrh > 512ull * 512ull ||
        (sbp & 31u) != 0u || (dbp & 31u) != 0u ||
        srcUnknown || dstUnknown || !src.valid || !dst.valid)
    {
        g_g627RejGeom.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    uint64_t srcLo = 0u, srcHi = 0u, dstLo = 0u, dstHi = 0u;
    if (!g627T8ByteSpan(sbp, sbw, ssax, ssay, rrw, rrh, srcLo, srcHi) ||
        !g627T8ByteSpan(dbp, dbw, dsax, dsay, rrw, rrh, dstLo, dstHi) ||
        dstHi + 1u > g_g261VramSize || srcHi + 1u > g_g261VramSize ||
        !(srcHi < dstLo || dstHi < srcLo))
    {
        g_g627RejGeom.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }

    const G627PlanKey key = g627MakeKey(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    const G627Plan *plan = g627GetPlan(key);
    if (plan == nullptr)
    {
        g_g627RejPlan.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }

    uint32_t mask = 0u;
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        // Conjunct 3: any dirty target the SOURCE reads makes VRAM's source bytes stale.
        if (!r.range.valid || (src.valid && g144RangeOverlaps(r.range, src)))
        {
            g_g627RejSrc.fetch_add(1u, std::memory_order_relaxed);
            return 0u;
        }
        const G627TargetPlan &tp = plan->targets[i];
        const bool dstHits = dst.valid && g144RangeOverlaps(r.range, dst);
        if (!dstHits)
            continue;
        // Conjunct 4: this target WILL be published by the caller unless the plan reconciles it.
        if (tp.texels.empty() || tp.oob || r.fbw == 0u || r.fbW <= 0 || r.fbW > 512 ||
            tp.tyHi < tp.tyLo || tp.tyHi >= 512)
        {
            g_g627RejCover.fetch_add(1u, std::memory_order_relaxed);
            return 0u;
        }
        mask |= 1u << i;
    }
    if (mask == 0u)
    {
        g_g627RejTarget.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    t_g627Pin.armed = true;
    t_g627Pin.mask = mask;
    t_g627Pin.key = key;
    g_g627Admits.fetch_add(1u, std::memory_order_relaxed);
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
        if (mask & (1u << i))
        {
            const G261Res &r = g_g261Res[i];
            if (r.dirtyHi >= r.dirtyLo)
                g_g627RowsSaved.fetch_add(
                    static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1),
                    std::memory_order_relaxed);
        }
    return mask;
}

// The publication this edge skipped is only sound if the FBO ends up holding the copy's bytes.
// Dropping the residency WITHOUT a readback is the only safe failure (a readback here would write
// the FBO's pre-copy bytes over the newer VRAM ones - G264's contract, verbatim).
static void g627DropResidency(uint32_t ti)
{
    G261Res &r = g_g261Res[ti];
    g280DropOverlays(static_cast<int>(ti));
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    g261UpdateWindow(static_cast<int>(ti));
    G178FbSnap &snap = g_g178FbSnap[kG261Fbp[ti]];
    snap.valid = false;
    snap.genSum = 0;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    g310NoteTargetClean(false);
}

static void g627Report()
{
    if (!g627StatOn())
        return;
    std::fprintf(stderr,
                 "[G627:mir] admits=%llu patches=%llu texels=%llu lanes=%llu rowsSaved=%llu "
                 "fail=%llu | rej(arm=%llu psm=%llu geom=%llu plan=%llu src=%llu cover=%llu "
                 "notarget=%llu) vfyChecks=%llu vfyBad=%llu\n",
                 static_cast<unsigned long long>(g_g627Admits.load()),
                 static_cast<unsigned long long>(g_g627Patches.load()),
                 static_cast<unsigned long long>(g_g627PatchTexels.load()),
                 static_cast<unsigned long long>(g_g627PatchLanes.load()),
                 static_cast<unsigned long long>(g_g627RowsSaved.load()),
                 static_cast<unsigned long long>(g_g627Fails.load()),
                 static_cast<unsigned long long>(g_g627RejArm.load()),
                 static_cast<unsigned long long>(g_g627RejPsm.load()),
                 static_cast<unsigned long long>(g_g627RejGeom.load()),
                 static_cast<unsigned long long>(g_g627RejPlan.load()),
                 static_cast<unsigned long long>(g_g627RejSrc.load()),
                 static_cast<unsigned long long>(g_g627RejCover.load()),
                 static_cast<unsigned long long>(g_g627RejTarget.load()),
                 static_cast<unsigned long long>(g_g627VfyChecks.load()),
                 static_cast<unsigned long long>(g_g627VfyBad.load()));
}

// The exactness oracle (DC2_G627_VERIFY=1). Reads the patched rect back out of the FBO and checks
// BOTH halves of the contract in one pass: every masked lane must now equal the byte guest VRAM
// holds (the copy's result), and every unmasked lane must still equal what the FBO held before the
// patch. A whole-word comparison could only ever see the first half.
static void g627VerifyPatch(uint32_t ti, int txLo, int glY, int w, int rows,
                            const std::vector<uint32_t> &before,
                            const std::vector<uint32_t> &val,
                            const std::vector<uint32_t> &mask)
{
    const G261Res &r = g_g261Res[ti];
    std::vector<uint32_t> after;
    if (!g178_backend_read_color(kG261Fbp[ti], r.fbW, 512, glY, rows, after) ||
        after.size() < static_cast<size_t>(r.fbW) * rows)
        return;
    for (int row = 0; row < rows; ++row)
        for (int c = 0; c < w; ++c)
        {
            const size_t li = static_cast<size_t>(row) * w + c;
            const uint32_t got = after[static_cast<size_t>(row) * r.fbW + txLo + c];
            const uint32_t m = mask[li];
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                const uint32_t mb = (m >> (lane * 8u)) & 0xFFu;
                const uint32_t want = mb != 0u
                                          ? ((val[li] >> (lane * 8u)) & 0xFFu)
                                          : ((before[li] >> (lane * 8u)) & 0xFFu);
                const uint32_t have = (got >> (lane * 8u)) & 0xFFu;
                g_g627VfyChecks.fetch_add(1u, std::memory_order_relaxed);
                if (want != have)
                {
                    const uint64_t bad = g_g627VfyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (bad <= 16u || (bad % 8192u) == 0u)
                        std::fprintf(stderr,
                                     "[G627:vfy-BAD] bad=%llu t=%03x tx=%d row=%d lane=%u "
                                     "want=0x%02x have=0x%02x mask=0x%08x\n",
                                     static_cast<unsigned long long>(bad), kG261Fbp[ti],
                                     txLo + c, row, lane, want, have, m);
                }
            }
        }
}

static void g627ApplyPendingL2lPatchImpl(uint8_t *vram, uint32_t vramSize)
{
    if (!t_g627Pin.armed)
        return;
    t_g627Pin.armed = false;
    const uint32_t mask = t_g627Pin.mask;
    const G627Plan *plan = g627GetPlan(t_g627Pin.key);
    if (vram == nullptr || plan == nullptr)
    {
        for (uint32_t i = 0u; i < kG248TargetCount; ++i)
            if (mask & (1u << i))
                g627DropResidency(i);
        g_g627Fails.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr, "[G627:patch-fail] stage=plan mask=0x%x\n", mask);
        return;
    }
    static std::vector<uint32_t> s_val, s_mask, s_before; // flush-owner thread only (G144 contract)
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        if ((mask & (1u << i)) == 0u)
            continue;
        G261Res &r = g_g261Res[i];
        const G627TargetPlan &tp = plan->targets[i];
        if (tp.texels.empty() || r.fbW <= 0 || r.fbW > 512 || r.fbw == 0u ||
            tp.tyLo < 0 || tp.tyHi >= 512 || tp.tyHi < tp.tyLo)
        {
            g627DropResidency(i);
            g_g627Fails.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G627:patch-fail] stage=shape t=%03x\n", kG261Fbp[i]);
            continue;
        }
        int txLo = 512, txHi = -1;
        for (const G627Texel &t : tp.texels)
        {
            if (static_cast<int>(t.tx) < txLo) txLo = t.tx;
            if (static_cast<int>(t.tx) > txHi) txHi = t.tx;
        }
        if (txHi < txLo || txHi >= r.fbW)
        {
            g627DropResidency(i);
            g_g627Fails.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G627:patch-fail] stage=bbox t=%03x\n", kG261Fbp[i]);
            continue;
        }
        const int w = txHi - txLo + 1;
        const int rows = tp.tyHi - tp.tyLo + 1;
        // GL rows are bottom-first (g264FlushMirror's convention): buffer row 0 is GS row tyHi.
        const int glY = 511 - tp.tyHi;
        const size_t n = static_cast<size_t>(w) * rows;
        s_val.assign(n, 0u);
        s_mask.assign(n, 0u);
        const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[i]);
        bool packed = true;
        uint64_t lanes = 0u;
        for (const G627Texel &t : tp.texels)
        {
            const size_t li = static_cast<size_t>(tp.tyHi - t.ty) * w +
                              static_cast<size_t>(t.tx - txLo);
            const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw, t.tx, t.ty);
            if (off + 4u > vramSize)
            {
                packed = false;
                break;
            }
            uint32_t word = 0u;
            std::memcpy(&word, vram + off, 4u);
            s_val[li] = word;
            uint32_t lm = 0u;
            for (uint32_t lane = 0; lane < 4u; ++lane)
                if (t.laneMask & (1u << lane))
                {
                    lm |= 0xFFu << (lane * 8u);
                    ++lanes;
                }
            s_mask[li] = lm;
        }
        if (g627VerifyOn())
        {
            s_before.assign(n, 0u);
            std::vector<uint32_t> pre;
            if (g178_backend_read_color(kG261Fbp[i], r.fbW, 512, glY, rows, pre) &&
                pre.size() >= static_cast<size_t>(r.fbW) * rows)
            {
                for (int row = 0; row < rows; ++row)
                    for (int c = 0; c < w; ++c)
                        s_before[static_cast<size_t>(row) * w + c] =
                            pre[static_cast<size_t>(row) * r.fbW + txLo + c];
            }
            else
            {
                s_before.clear();
            }
        }
        if (!packed ||
            !g627_backend_write_color_rect_masked(kG261Fbp[i], r.fbW, 512, txLo, glY,
                                                  w, rows, s_val, s_mask))
        {
            g627DropResidency(i);
            g_g627Fails.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G627:patch-fail] stage=%s t=%03x rect=%d,%d %dx%d fbW=%d\n",
                         packed ? "backend" : "vram", kG261Fbp[i], txLo, tp.tyLo, w, rows,
                         r.fbW);
            continue;
        }
        if (g627VerifyOn() && !s_before.empty())
            g627VerifyPatch(i, txLo, glY, w, rows, s_before, s_val, s_mask);
        g_g627Patches.fetch_add(1u, std::memory_order_relaxed);
        g_g627PatchTexels.fetch_add(tp.texels.size(), std::memory_order_relaxed);
        g_g627PatchLanes.fetch_add(lanes, std::memory_order_relaxed);

        // ===== residency bookkeeping, identical in intent to g264FlushMirror's tail =====
        if (tp.tyLo < r.covLo) r.covLo = tp.tyLo;
        if (tp.tyHi > r.covHi) r.covHi = tp.tyHi;
        // G597: forgive the copy's own VRAM generation bump for the PATCHED page rows only. Every
        // other page in the claimed window may hold a genuine unreconciled guest write and must keep
        // its older anchor, or G536's guard is disarmed exactly where it matters.
        if (g597ScopedAnchorOn() && r.g536PageGenFbw == r.fbw && r.fbw != 0u && r.fbw <= 8u)
        {
            const std::array<uint64_t, 16 * 8> savedGens = r.g536PageGen;
            const uint32_t savedFbw = r.g536PageGenFbw;
            g261UpdateWindow(static_cast<int>(i));
            if (r.g536PageGenFbw == savedFbw && r.g536PageGenFbw == r.fbw)
            {
                const uint32_t prLo = static_cast<uint32_t>(std::max(0, tp.tyLo)) / 32u;
                const uint32_t prHi = static_cast<uint32_t>(std::min(511, tp.tyHi)) / 32u;
                for (uint32_t py = 0; py < 16u; ++py)
                {
                    if (py >= prLo && py <= prHi)
                        continue;
                    for (uint32_t px = 0; px < r.fbw; ++px)
                    {
                        const uint32_t p = py * r.fbw + px;
                        if (p < r.g536PageGen.size())
                            r.g536PageGen[p] = savedGens[p];
                    }
                }
            }
        }
        else
        {
            g261UpdateWindow(static_cast<int>(i));
        }
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
        if (snap.valid)
        {
            if (tp.tyLo < snap.rowLo) snap.rowLo = tp.tyLo;
            if (tp.tyHi > snap.rowHi) snap.rowHi = tp.tyHi;
            snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                         static_cast<uint32_t>(r.fbW), 512u);
        }
        g280NoteMirrorPatch(static_cast<int>(i), tp.tyLo, tp.tyHi);
        g310NoteProducerWrite(static_cast<int>(i), tp.tyLo, tp.tyHi);
    }
    if (g627StatOn())
    {
        const uint64_t n = g_g627Patches.load(std::memory_order_relaxed);
        if (n <= 4u || (n % 512u) == 0u)
            g627Report();
    }
}
