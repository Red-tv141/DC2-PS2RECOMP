// ---- G340: lazy-VRAM payoff-ceiling census (behavior-pure, default-off DC2_G340_CENSUS=1) -----
// The arc's #1 unexplored lever (lazy materialized guest VRAM) proposes deferring the per-target
// materialize glReadPixels until a guest/runtime consumer actually READS the guest-VRAM bytes,
// via page protection. But `g261Materialize` ALREADY defers readback to consumer edges — it fires
// only when a consumer's block range overlaps a dirty resident target. So a page-fault lazy scheme
// can only save the readback for dirty rows that NO consumer actually reads before they are
// overwritten. This census measures that ceiling directly, without any SEH/VirtualProtect risk:
// at each range-triggered materialize, split the target's dirty rows (the readback unit,
// == g_g279MatRows) three ways —
//   consumed : the triggering copy's SRC rect dirty-overlaps them  -> a real read; lazy CANNOT skip.
//   overwrite: only the DST rect dirty-overlaps them (no src)      -> written over; lazy skips IFF
//              the write fully covers (upper-bounded skippable).
//   conserv  : NEITHER src nor dst dirty-overlaps (materialize fired purely on the coarse
//              full-target range) -> pure conservative barrier; 100% skippable by an exact scheme.
// unknown    : srcUnknown/dstUnknown fail-closed (counted as consumed = safe "needed" upper bound).
// Skippable = overwrite + conserv. If consumed dominates, lazy-VRAM is the G280/G304/G339 local
// minimum (deferral just relocates the read); if skippable dominates, a page-exact scheme pays.
static bool g340CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G340_CENSUS");
    return s_on;
}
static std::atomic<uint64_t> g_g340Mats{0};        // range-triggered materializes censused
static std::atomic<uint64_t> g_g340RowsTotal{0};   // Σ dirty rows materialized (== consumed+skippable)
static std::atomic<uint64_t> g_g340RowsConsumed{0};// Σ rows the consumer src-reads (lazy cannot skip)
static std::atomic<uint64_t> g_g340RowsOverwrite{0};// Σ rows only dst-written (skippable if write total)
static std::atomic<uint64_t> g_g340RowsConserv{0}; // Σ rows neither src nor dst dirty-overlap (dead)
static std::atomic<uint64_t> g_g340RowsUnknown{0}; // Σ rows under fail-closed unknown ranges
static std::atomic<uint64_t> g_g340CauseTotal[8]{};    // per-cause total rows
static std::atomic<uint64_t> g_g340CauseSkippable[8]{};// per-cause (overwrite+conserv) rows

static bool g267CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G267_CENSUS");
    return s_on;
}

static void g266Report(); // defined below g261AnyDirty (reads the G261 residency state)

// G291 (bring-up opt-in DC2_G291_ATLAS=1; kills DC2_G291_NO_ATLAS=1 / DC2_G291_ATLAS=0):
// persistent-residency consumer-completeness for the 0x139 work-block atlas. The G290-ON census
// measured the exact relocation: admitting the seeded fbp=0x139/fbw=8 batch keeps the whole
// atlas dirty-resident, and texture consumers (T8 materials at 0x34a0/0x3460/0x34e0, the T8
// 0x143<-0x13c view source) then force kG261MatTexAlias materializations of ~4x more rows
// (c5 3.7s -> 11.8s per run) — even though the game re-UPLOADS those exact texture pages every
// frame (3.9 MB/f) before sampling them, so guest VRAM is already current for the pages the
// consumers read. G291 tracks cumulative per-pixel upload re-coverage: a page whose pixels were
// FULLY re-covered by noted uploads since the last wave write has VRAM >= FBO by construction
// (the G264 mirror map records the exact CT32 byte->target-pixel map of every upload), so a
// texture/CLUT read overlapping ONLY such pages needs no materialization. Writers still publish
// through every existing edge; the skip applies to the READ-triggered materialize only, and
// fails closed on any page not provably current or any layout change. G292 audited the G280
// alias-view write paths and retained exact tile-coverage invalidation diagnostics, but the
// combined behavior remains forbidden: true multi-owner pages need a per-target authority token,
// which G283's page-global epoch does not provide. G291 implies the G290 seeded admission.
static bool g291AtlasOn()
{
    // G294 combined arm: the owner-token makes the g280 alias-view exact on multi-owner pages, so
    // G291's upload-re-coverage retire (excluded from G280 since G292) can now run ALONGSIDE it —
    // this is the consumer-chain retire that makes the atlas residency actually pay. g294 also
    // implies the retire (and, via g290ExactRowsOn, the seeded admission) so the whole paying arm
    // is one behavior lever: DC2_G294_OWNER_TOKEN=1.
    // G310 removes the whole-atlas display publication that made this exact retire neutral.
    // Its subordinate-alias retirement makes G290's seeded 0x139 admission single-owner again,
    // so the proven re-coverage rule completes the remaining T8 consumer class.
    static const bool s_on = (envFlagEnabled("DC2_G291_ATLAS") ||
                              g294OwnerTokenRequested() ||
                              g310AliasRetireOn()) &&
                             !envFlagEnabled("DC2_G291_NO_ATLAS") &&
                             !envFlagDisabled("DC2_G291_ATLAS");
    return s_on && (!g280AliasOn() || g294OwnerTokenOn());
}
static bool g291StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G291_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g291TexSkips{0}, g_g291TexMats{0}, g_g291CurPagesSet{0},
    g_g291CurPagesClr{0}, g_g291LayoutResets{0};

// G292: any GPU-side alias copy makes this target tile potentially newer than guest VRAM.
// Clear only the exact CT32 64x32 page tile; unrelated uploaded pages in the same row retain
// their G291 proof. Overlay restoration later makes FBO==VRAM again, but leaving the tile clear
// is the conservative direction until a noted upload re-covers it.
static void g291ClearCopiedTile(int ti, uint32_t pageRow, uint32_t pageCol)
{
    if (!g291AtlasOn() || ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    G261Res &r = g_g261Res[ti];
    if (r.g291CovFbw == 0u)
        return;
    if (pageRow >= 16u || pageCol >= 8u)
    {
        r.g291Cov = {};
        r.g291CovFbw = 0u;
        g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t y0 = pageRow * 32u;
    for (uint32_t y = y0; y < y0 + 32u; ++y)
        r.g291Cov[y][pageCol] = 0u;
    g_g291CurPagesClr.fetch_add(64u * 32u, std::memory_order_relaxed);
}

static void g261Report()
{
    if (!g261StatOn() && !g279ProfileOn() && !g310StatOn() && !g340CensusOn())
        return;
    std::fprintf(stderr,
                 "[G536:pageown] on=%d publications=%llu guarded=%llu pagesKept=%llu\n",
                 g536PageOwnOn() ? 1 : 0,
                 (unsigned long long)g_g536PubTotal.load(std::memory_order_relaxed),
                 (unsigned long long)g_g536PubGuarded.load(std::memory_order_relaxed),
                 (unsigned long long)g_g536PagesKept.load(std::memory_order_relaxed));
    if (g291StatOn())
        std::fprintf(stderr,
                     "[G291:stat] skips=%llu mats=%llu covSetPx=%llu covClrPx=%llu "
                     "layoutResets=%llu\n",
                     (unsigned long long)g_g291TexSkips.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291TexMats.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesSet.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291CurPagesClr.load(std::memory_order_relaxed),
                     (unsigned long long)g_g291LayoutResets.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G261:stat] waves=%llu rbSkip=%llu fboBind=%llu fboBindBi=%llu fboRej=%llu inv=%llu unk=%llu "
                 "mat(cpu=%llu inl=%llu up=%llu l2l=%llu l2h=%llu tex=%llu rsync=%llu fail=%llu) "
                 "mir(n=%llu f=%llu x=%llu) g267(cand=%llu elig=%llu foot=%llu alias=%llu)\n",
                 (unsigned long long)g_g261Waves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261SkippedRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboBinds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g263FboBindBi.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261FboRejects.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Invariants.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261MatUnknownRange.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g261Mat[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirNoted.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFlushed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g264MirFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriCandidates.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriEligible.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriFootReject.load(std::memory_order_relaxed),
                 (unsigned long long)g_g267TriAliasReject.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G261:res]");
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        std::fprintf(stderr, " %03x(mat=%llu win=%d..%d dirty=%d)", kG261Fbp[i],
                     (unsigned long long)g_g261MatByTarget[i].load(std::memory_order_relaxed),
                     g_g261Res[i].winLo, g_g261Res[i].winHi, g_g261Res[i].dirty ? 1 : 0);
    std::fprintf(stderr, " rttRej(classify=%llu depth=%llu) g262(zwave=%llu zrb=%llu)\n",
                 (unsigned long long)g_g261RttClassifyRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g261RttDepthRej.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262DepthWaves.load(std::memory_order_relaxed),
                 (unsigned long long)g_g262ZReadbacks.load(std::memory_order_relaxed));
    g280Report();
    g310Report();
    if (g274ZTimeOn())
        std::fprintf(stderr,
                     "[G274:ztime] zUpMs=%.1f zRbMs=%.1f redundant=%llu necessary=%llu "
                     "(cumulative swizzle round-trip; redundant=elidable by depth residency)\n",
                     (double)g_g274ZUpNs.load(std::memory_order_relaxed) / 1e6,
                     (double)g_g274ZRbNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274ZRedundant.load(std::memory_order_relaxed),
                     (unsigned long long)g_g274ZNecessary.load(std::memory_order_relaxed));
    if (g274ZTimeOn())
        std::fprintf(stderr, "[G274:submit] submitMs=%.1f submitN=%llu (synchronous backend waves)\n",
                     (double)g_g274SubNs.load(std::memory_order_relaxed) / 1e6,
                     (unsigned long long)g_g274SubN.load(std::memory_order_relaxed));
    if (g274ZTimeOn() || g274ZResidentOn())
        std::fprintf(stderr, "[G274:zresident] uploadsSkipped=%llu (redundant Z re-uploads elided)\n",
                     (unsigned long long)g_g274ZUploadsSkipped.load(std::memory_order_relaxed));
    if (g275StatOn())
        std::fprintf(stderr,
                     "[G275:fused] jobs=%llu readWaitsElided=%llu submitReadMs=%.1f "
                     "zSwizzleLanes=%d\n",
                     (unsigned long long)g_g275FusedJobs.load(std::memory_order_relaxed),
                     (unsigned long long)g_g275ReadWaitsElided.load(std::memory_order_relaxed),
                     (double)g_g275FusedNs.load(std::memory_order_relaxed) / 1e6,
                     g275ZSwizzleLanes());
    if (g276ProbeOn())
    {
        auto ms = [](const std::atomic<uint64_t> &a) {
            return (double)a.load(std::memory_order_relaxed) / 1e6;
        };
        auto cnt = [](const std::atomic<uint64_t> &a) {
            return (unsigned long long)a.load(std::memory_order_relaxed);
        };
        std::fprintf(stderr,
                     "[G276:submit] rttSkip(depthW=%.1fms/%llu noZ=%.1fms/%llu) "
                     "readback(colorZ=%.1fms/%llu color=%.1fms/%llu)\n",
                     ms(g_g276SubNs[1][1]), cnt(g_g276SubN[1][1]),
                     ms(g_g276SubNs[1][0]), cnt(g_g276SubN[1][0]),
                     ms(g_g276SubNs[0][1]), cnt(g_g276SubN[0][1]),
                     ms(g_g276SubNs[0][0]), cnt(g_g276SubN[0][0]));
        const unsigned long long win = cnt(g_g276ColorRbWinRows);
        const unsigned long long full = cnt(g_g276ColorRbFullRows);
        std::fprintf(stderr,
                     "[G276:colorrb] n=%llu winRows=%llu fullRows=%llu span=%.1f%% "
                     "(rows actually swizzled vs full-FBO glReadPixels)\n",
                     cnt(g_g276ColorRbN), win, full,
                     full ? 100.0 * (double)win / (double)full : 0.0);
    }
    if (g276DisplayRbOn() || g276ProbeOn())
    {
        const unsigned long long def = g_g276Deferred.load(std::memory_order_relaxed);
        const unsigned long long fl = g_g276Flushes.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G276:coalesce] deferred=%llu flushes=%llu rows=%llu fail=%llu "
                     "readbacksSaved=%llu uploadPublish=%llu exactGenWin=%llu\n",
                     def, fl, (unsigned long long)g_g276FlushRows.load(std::memory_order_relaxed),
                     (unsigned long long)g_g276FlushFail.load(std::memory_order_relaxed),
                     def > fl ? def - fl : 0ull,
                     (unsigned long long)g_g277UploadPublish.load(std::memory_order_relaxed),
                     (unsigned long long)g_g277ExactGenWin.load(std::memory_order_relaxed));
    }
    if (g279ProfileOn())
    {
        std::fprintf(stderr, "[G279:mat]");
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llur", i,
                         (double)g_g279MatNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279MatReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279MatRows[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/rows)\n[G279:display]");
        for (int i = 0; i < 9; ++i)
            std::fprintf(stderr, " c%d=%.1f/%.1fms/%llu", i,
                         (double)g_g279DisplayNs[i].load(std::memory_order_relaxed) / 1e6,
                         (double)g_g279DisplayReadNs[i].load(std::memory_order_relaxed) / 1e6,
                         (unsigned long long)g_g279DisplayN[i].load(std::memory_order_relaxed));
        std::fprintf(stderr, " (total/read/count)\n");
    }
    if (g340CensusOn())
    {
        const uint64_t tot = g_g340RowsTotal.load(std::memory_order_relaxed);
        const uint64_t cons = g_g340RowsConsumed.load(std::memory_order_relaxed);
        const uint64_t ovr = g_g340RowsOverwrite.load(std::memory_order_relaxed);
        const uint64_t con = g_g340RowsConserv.load(std::memory_order_relaxed);
        const uint64_t unk = g_g340RowsUnknown.load(std::memory_order_relaxed);
        const uint64_t skip = ovr + con;
        std::fprintf(stderr,
                     "[G340:ceiling] mats=%llu rows=%llu consumed=%llu overwrite=%llu "
                     "conserv=%llu unknown=%llu skippable=%llu (%.1f%% of rows)\n",
                     (unsigned long long)g_g340Mats.load(std::memory_order_relaxed),
                     (unsigned long long)tot, (unsigned long long)cons,
                     (unsigned long long)ovr, (unsigned long long)con,
                     (unsigned long long)unk, (unsigned long long)skip,
                     tot ? 100.0 * (double)skip / (double)tot : 0.0);
        std::fprintf(stderr, "[G340:bycause]");
        for (int i = 0; i < 8; ++i)
        {
            const uint64_t ct = g_g340CauseTotal[i].load(std::memory_order_relaxed);
            if (ct == 0u)
                continue;
            const uint64_t cs = g_g340CauseSkippable[i].load(std::memory_order_relaxed);
            std::fprintf(stderr, " c%d=%llu/%lluskip", i,
                         (unsigned long long)cs, (unsigned long long)ct);
        }
        std::fprintf(stderr, " (skippable/total rows; c2=upload c3=l2l)\n");
    }
    if (g284StatOn())
    {
        std::fprintf(stderr, "[G284:stat] uploadCoalesce=%d skipped=%llu flushed=%llu\n",
                     g284UploadCoalesceOn() ? 1 : 0,
                     (unsigned long long)g_g284UploadSkipped.load(std::memory_order_relaxed),
                     (unsigned long long)g_g284UploadFlushed.load(std::memory_order_relaxed));
    }
    if (g288StatOn())
    {
        std::fprintf(stderr,
                     "[G288:stat] audits=%llu publishes=%llu disjoint=%llu unknown=%llu\n",
                     (unsigned long long)g_g288GpuAudits.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuDisjoint.load(std::memory_order_relaxed),
                     (unsigned long long)g_g288GpuUnknown.load(std::memory_order_relaxed));
    }
    if (g289StatOn())
    {
        std::fprintf(stderr,
                     "[G289:stat] direct=%llu copy=%llu copyFail=%llu consume=%llu carry=%llu "
                     "view=%llu viewFail=%llu bind=%llu mat=%llu matFail=%llu unexpected=%llu "
                     "upOverwrite=%llu inlineCarry=%llu frameCarry=%llu suffixPub=%llu "
                     "verify=%llu bad=%llu "
                     "active=%u version=%llu owned=%x..%x\n",
                     (unsigned long long)g_g289Direct.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Copies.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289CopyFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Consumes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Carries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViews.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerViewFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289OwnerBinds.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Materializes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289MatFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289Unexpected.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289UploadOverwrites.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289InlineCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289FrameCarries.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289SuffixPublishes.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyChecks.load(std::memory_order_relaxed),
                     (unsigned long long)g_g289VerifyBad.load(std::memory_order_relaxed),
                     s_g289Owner.active ? 1u : 0u,
                     (unsigned long long)s_g289Owner.version,
                     s_g289Owner.firstOwnedBlock, s_g289Owner.lastOwnedBlock);
    }
    if (g338Ct24ViewOn())
        std::fprintf(stderr, "[G338:stat] views=%llu buildFail=%llu verifyBad=%llu\n",
                     (unsigned long long)g_g338Views.load(std::memory_order_relaxed),
                     (unsigned long long)g_g338BuildFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g338VerifyBad.load(std::memory_order_relaxed));
    g345PrintEscape();
    g347PrintUp139();
    g590Report(); // G590 surface-authority ledger; no-op unless DC2_G590_CENSUS/SHADOW
    g591Report(); // G591 window-authority censuses + same-run oracles; no-op unless selected
    g262Report();
    g265Report();
    g266Report();
}

static bool g261AnyDirty()
{
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        if (g_g261Res[i].dirty)
            return true;
    return false;
}

// G266: a CPU band replay is about to materialize resident target `ti` because entry `e`
// overlaps it. hitBits: 1=color 2=depth 4=texture 8=CLUT 16=unknown-range.
static void g266NoteCpuMat(const G144Entry &e, uint32_t ti, uint32_t hitBits)
{
    if (!g266CensusOn())
        return;
    g_g266CpuN.fetch_add(1u, std::memory_order_relaxed);
    const GSContext &ctx = e.ctx;
    const bool zOn = ((ctx.test >> 16) & 1u) != 0u && ((ctx.zbuf >> 32) & 1ull) == 0ull;
    uint64_t k = 0;
    k |= static_cast<uint64_t>(ctx.frame.fbp & 0x1FFu);                               // 0-8
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 9;                              // 9-11
    k |= static_cast<uint64_t>(e.prim.tme ? 1u : 0u) << 12;                           // 12
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.psm & 0x3Fu) : 0x3Fu) << 13;    // 13-18
    k |= static_cast<uint64_t>(ctx.zbuf & 0x1FFu) << 19;                              // 19-27
    k |= static_cast<uint64_t>(zOn ? 1u : 0u) << 28;                                  // 28
    k |= static_cast<uint64_t>(hitBits & 0x1Fu) << 29;                                // 29-33
    k |= static_cast<uint64_t>(ti & 0x7u) << 34;                                      // 34-36
    k |= static_cast<uint64_t>(g_g266FlushRej & 0xFu) << 37;                          // 37-40
    k |= static_cast<uint64_t>((g_g266FlushRej == kG266FrClassify
                                    ? g_g266ClassifySite : 0xF) & 0xFu) << 41;        // 41-44
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.tbp0 & 0x3FFFu) : 0u) << 45;    // 45-58
    ++g_g266CpuShapes[k];
    static std::atomic<uint32_t> s_log{0};
    if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
        std::fprintf(stderr,
                     "[G266:cpu] fbp=%03x prim=%u tme=%u tpsm=0x%x tbp=0x%x zbp=%03x zOn=%u "
                     "hit=0x%x t=%03x rej=%s site=%s\n",
                     ctx.frame.fbp, static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                     e.prim.tme ? ctx.tex0.psm : 0u, e.prim.tme ? ctx.tex0.tbp0 : 0u,
                     static_cast<unsigned>(ctx.zbuf & 0x1FFu), zOn ? 1u : 0u, hitBits,
                     kG261Fbp[ti], kG266FrName[g_g266FlushRej],
                     (g_g266FlushRej == kG266FrClassify && g_g266ClassifySite >= 0 &&
                      g_g266ClassifySite < kG262RejSiteCount)
                         ? kG262SiteName[g_g266ClassifySite]
                         : "-");
}

// G266: a local→local copy is about to drain resident targets. Record each target's exact role
// (source/destination/unknown-range) plus both transfer layouts — the G264 lesson says the fix
// (if any) must map addresses under BOTH swizzles, so the census must carry both.
static void g266NoteLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                              uint32_t ssax, uint32_t ssay,
                              uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                              uint32_t dsax, uint32_t dsay,
                              uint32_t rrw, uint32_t rrh,
                              const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown)
{
    if (!g266CensusOn() || !g261WaveOn())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        const bool unknown = srcUnknown || dstUnknown || !r.range.valid;
        const bool hitSrc = !unknown && src.valid && g144RangeOverlaps(r.range, src);
        const bool hitDst = !unknown && dst.valid && g144RangeOverlaps(r.range, dst);
        if (!unknown && !hitSrc && !hitDst)
            continue; // this target survives the copy — not a drain
        g_g266L2lN.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t role = unknown ? 4u : ((hitSrc ? 1u : 0u) | (hitDst ? 2u : 0u));
        uint64_t k = 0;
        k |= static_cast<uint64_t>(sbp & 0x3FFFu);       // 0-13
        k |= static_cast<uint64_t>(spsm & 0x3Fu) << 14;  // 14-19
        k |= static_cast<uint64_t>(sbw & 0x3Fu) << 20;   // 20-25
        k |= static_cast<uint64_t>(dbp & 0x3FFFu) << 26; // 26-39
        k |= static_cast<uint64_t>(dpsm & 0x3Fu) << 40;  // 40-45
        k |= static_cast<uint64_t>(dbw & 0x3Fu) << 46;   // 46-51
        k |= static_cast<uint64_t>(i & 0x7u) << 52;      // 52-54
        k |= static_cast<uint64_t>(role & 0x7u) << 55;   // 55-57
        ++g_g266L2lShapes[k];
        static std::atomic<uint32_t> s_log{0};
        if (s_log.fetch_add(1u, std::memory_order_relaxed) < 24u)
            std::fprintf(stderr,
                         "[G266:l2l] s(bp=0x%x bw=%u psm=0x%x at %u,%u) d(bp=0x%x bw=%u psm=0x%x "
                         "at %u,%u) rect=%ux%u t=%03x role=%s dirty=%d..%d cov=%d..%d\n",
                         sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                         dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh,
                         kG261Fbp[i],
                         role == 4u ? "unk" : role == 3u ? "src+dst"
                                            : role == 1u ? "src" : "dst",
                         r.dirtyLo, r.dirtyHi, r.covLo, r.covHi);
    }
}

// G266: a GPU batch samples a resident target but the FBO-direct bind was rejected — record the
// consumer's exact state shape (the [G262:bindrej] print is capped; this aggregates all of them).
static void g266NoteTexBindMat(uint32_t dstFbp, uint32_t ti, const G144Entry &e,
                               const G178EntryState &st)
{
    if (!g266CensusOn())
        return;
    g_g266TexN.fetch_add(1u, std::memory_order_relaxed);
    uint64_t k = 0;
    k |= static_cast<uint64_t>(dstFbp & 0x1FFu);                 // 0-8
    k |= static_cast<uint64_t>(ti & 0x7u) << 9;                  // 9-11
    k |= static_cast<uint64_t>(e.ctx.tex0.psm & 0x3Fu) << 12;    // 12-17
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 18;        // 18-20
    k |= static_cast<uint64_t>(e.prim.fst ? 1u : 0u) << 21;      // 21
    k |= static_cast<uint64_t>(st.bilinear ? 1u : 0u) << 22;     // 22
    k |= static_cast<uint64_t>(st.wrapU & 0x3u) << 23;           // 23-24
    k |= static_cast<uint64_t>(st.wrapV & 0x3u) << 25;           // 25-26
    k |= static_cast<uint64_t>(e.ctx.tex0.tbp0 & 0x3FFFu) << 27; // 27-40
    k |= static_cast<uint64_t>(e.ctx.tex0.tbw & 0x3Fu) << 41;    // 41-46
    ++g_g266TexShapes[k];
}

static void g266Report()
{
    if (!g266CensusOn())
        return;
    std::fprintf(stderr, "[G266:census] cpu=%llu l2l=%llu tex=%llu shapes(%zu/%zu/%zu)\n",
                 (unsigned long long)g_g266CpuN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266L2lN.load(std::memory_order_relaxed),
                 (unsigned long long)g_g266TexN.load(std::memory_order_relaxed),
                 g_g266CpuShapes.size(), g_g266L2lShapes.size(), g_g266TexShapes.size());
    for (const auto &kv : g_g266CpuShapes)
    {
        const uint64_t k = kv.first;
        const int rej = static_cast<int>((k >> 37) & 0xFu);
        const int site = static_cast<int>((k >> 41) & 0xFu);
        std::fprintf(stderr,
                     "[G266:cpuShape] n=%llu fbp=%03x prim=%u tme=%u tpsm=0x%02x tbp=0x%x "
                     "zbp=%03x zOn=%u hit=0x%x t=%03x rej=%s site=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), (unsigned)((k >> 9) & 0x7u),
                     (unsigned)((k >> 12) & 1u), (unsigned)((k >> 13) & 0x3Fu),
                     (unsigned)((k >> 45) & 0x3FFFu), (unsigned)((k >> 19) & 0x1FFu),
                     (unsigned)((k >> 28) & 1u), (unsigned)((k >> 29) & 0x1Fu),
                     kG261Fbp[(k >> 34) & 0x7u],
                     kG266FrName[rej],
                     (rej == kG266FrClassify && site < kG262RejSiteCount)
                         ? kG262SiteName[site] : "-");
    }
    for (const auto &kv : g_g266L2lShapes)
    {
        const uint64_t k = kv.first;
        const uint32_t role = static_cast<uint32_t>((k >> 55) & 0x7u);
        std::fprintf(stderr,
                     "[G266:l2lShape] n=%llu s(bp=0x%x bw=%u psm=0x%02x) d(bp=0x%x bw=%u "
                     "psm=0x%02x) t=%03x role=%s\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x3FFFu), (unsigned)((k >> 20) & 0x3Fu),
                     (unsigned)((k >> 14) & 0x3Fu),
                     (unsigned)((k >> 26) & 0x3FFFu), (unsigned)((k >> 46) & 0x3Fu),
                     (unsigned)((k >> 40) & 0x3Fu),
                     kG261Fbp[(k >> 52) & 0x7u],
                     role == 4u ? "unk" : role == 3u ? "src+dst"
                                        : role == 1u ? "src" : "dst");
    }
    for (const auto &kv : g_g266TexShapes)
    {
        const uint64_t k = kv.first;
        std::fprintf(stderr,
                     "[G266:texShape] n=%llu dstFbp=%03x t=%03x tpsm=0x%02x tbp=0x%x tbw=%u "
                     "prim=%u fst=%u bilin=%u wrap=%u%u\n",
                     (unsigned long long)kv.second,
                     (unsigned)(k & 0x1FFu), kG261Fbp[(k >> 9) & 0x7u],
                     (unsigned)((k >> 12) & 0x3Fu), (unsigned)((k >> 27) & 0x3FFFu),
                     (unsigned)((k >> 41) & 0x3Fu), (unsigned)((k >> 18) & 0x7u),
                     (unsigned)((k >> 21) & 1u), (unsigned)((k >> 22) & 1u),
                     (unsigned)((k >> 23) & 0x3u), (unsigned)((k >> 25) & 0x3u));
    }
}

// Recompute the claimed row window (cov ∪ dirty) and its block range + gen baseline. Rows
// OUTSIDE the window belong to VRAM alone (never uploaded to or read back from the FBO), so
// writes there are free — no materialize, no invariant. The gen baseline is scoped to the same
// window so unrelated same-fbp-page-range traffic cannot trip it falsely.
// G536: per-page twin of `genAtSkip`, anchored at the same instant (defined below with the rest
// of the page-ownership guard).
static void g536SnapshotPageGens(G261Res &r, uint32_t fbp);

// ===== G597: is the guard's ANCHOR destroyed between the hazard and the guard? =====
// G597's first probe refuted the fbw-flip hypothesis: `[G597:own]` reads `ran=calls` with EVERY
// conjunct passing, and the walk still finds 0.07% of pages moved. So the guard is not disarmed —
// its BASELINE is stale-free, i.e. re-anchored too late. `g261Materialize` calls `g264FlushMirror`
// BEFORE the readback, and that function ends in `g261UpdateWindow`, which re-snapshots ALL 128
// pages even though the mirror only patched `patchLo..patchHi`. Every guest write elsewhere in the
// window is forgiven, moments before `g536CollectGuestOwnedPages` asks about exactly those pages.
// This counts re-anchors that land inside a materialize BEFORE its publication.
static bool g597OwnChkOn();                 // defined with the rest of the probe, below
static bool g597ScopedAnchorOn();           // the FIX's gate, likewise
static thread_local bool t_g597InMatPrePub = false;
static std::atomic<uint64_t> g_g597AnchorTotal{0}, g_g597AnchorPrePub{0},
    g_g597MirrorAnchor{0}, g_g597MirrorScoped{0};

static void g261UpdateWindow(int ti)
{
    if (g597OwnChkOn())
    {
        g_g597AnchorTotal.fetch_add(1u, std::memory_order_relaxed);
        if (t_g597InMatPrePub)
            g_g597AnchorPrePub.fetch_add(1u, std::memory_order_relaxed);
    }
    G261Res &r = g_g261Res[ti];
    int lo = r.covLo, hi = r.covHi;
    if (r.dirty)
    {
        lo = std::min(lo, r.dirtyLo);
        hi = std::max(hi, r.dirtyHi);
    }
    r.winLo = lo;
    r.winHi = hi;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[ti]);
    if (hi >= lo && r.fbW > 0)
    {
        r.range = g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                                   static_cast<uint32_t>(lo),
                                   static_cast<uint32_t>(r.fbW),
                                   static_cast<uint32_t>(hi - lo + 1));
        r.genAtSkip = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                                     static_cast<uint32_t>(lo),
                                     static_cast<uint32_t>(r.fbW),
                                     static_cast<uint32_t>(hi - lo + 1));
        g536SnapshotPageGens(r, kG261Fbp[ti]); // G536: per-page twin of genAtSkip
    }
    else
    {
        r.range = G144BlockRange{};
        r.genAtSkip = 0;
        r.g536PageGenFbw = 0;
    }
}

static uint64_t g261WindowGenNow(const G261Res &r, uint32_t fbp)
{
    if (r.winHi < r.winLo || r.fbW <= 0)
        return 0;
    return g178GenSumRect(framePageBaseToBlock(fbp), r.fbw, GS_PSM_CT32, 0u,
                          static_cast<uint32_t>(r.winLo),
                          static_cast<uint32_t>(r.fbW),
                          static_cast<uint32_t>(r.winHi - r.winLo + 1));
}

// ===== G536 probe (default-OFF, DC2_G536_MATCHK=1): is guest VRAM NEWER at publication time? =====
// The residency contract's escaped-writer invariant (genAtSkip vs g261WindowGenNow) is tested only
// where a NEW wave decides uploadFb. It is never re-tested at g261Materialize, i.e. at the moment
// the FBO is written back OVER guest VRAM. If a guest writer touched the claimed window between
// the GPU render and that publication, the publication silently overwrites the newer guest bytes.
// On MAP-15 target 0x139 is resident at fbw=8 cov=0..447, so its claim spans ~blocks 0x2720..0x3520
// and swallows the T8 cloud texture at 0x2720 plus several streaming-texture pages.
// Counters only; no behaviour change.
static std::atomic<uint64_t> g_g536MatTotal{0}, g_g536MatMoved{0};
static bool g536MatChkOn()
{
    static const bool on = envFlagEnabled("DC2_G536_MATCHK");
    return on;
}
// ===== G536 FIX (default-ON, kill DC2_G536_NO_PAGEOWN=1): page-ownership-aware publication =====
// The residency claims a full-width row window, so on MAP-15 target 0x139 (fbw=8, cov=0..447)
// its claim spans ~blocks 0x2720..0x3520 and swallows the T8 cloud texture at 0x2720 plus several
// per-frame streaming-texture pages. `g261Materialize` then writes that window back from the FBO
// at FULL WIDTH. That round trip is only the identity while every guest write inside the window
// has been reconciled into the FBO (the G264 mirror does that for the CT32 host->local uploads it
// accepts, and re-anchors the baseline afterwards). Any OTHER guest write leaves guest VRAM newer,
// and the publication silently overwrites it with render-target pixels — which the sky's T8 cloud
// draw then decodes as achromatic garbage, one PSMT8 page at a time.
//
// The rule: publish the FBO only for pages the FBO still owns. A page whose write generation moved
// since the anchor is owned by the guest; its bytes are saved before the unpack and restored after,
// so the publication becomes exact per page instead of correct-per-window.
// Snapshot the per-page generations of the target's claimed pages. Called wherever `genAtSkip`
// is anchored, so the two always describe the same instant.
static void g536SnapshotPageGens(G261Res &r, uint32_t fbp)
{
    if (r.fbw == 0u || r.fbw > 8u)
    {
        r.g536PageGenFbw = 0u;
        return;
    }
    const uint32_t basePage = fbp; // FRAME fbp is already a GS page index (block = fbp * 32)
    const uint32_t pages = 16u * r.fbw;
    for (uint32_t p = 0; p < pages && p < r.g536PageGen.size(); ++p)
    {
        const uint32_t page = basePage + p;
        r.g536PageGen[p] = page < kG178PageCount
                               ? g_g178PageGen[page].load(std::memory_order_relaxed)
                               : 0u;
    }
    r.g536PageGenFbw = r.fbw;
}

// Bytes of one 8 KB GS page, saved so a publication cannot destroy guest-owned content.
struct G536PageSave
{
    uint32_t page;
    std::array<uint8_t, 8192> bytes;
};

// True exactly when G536's publication guard will preserve this target-local CT32 page instead
// of allowing the FBO readback to replace it. Atlas composition uses the same predicate so its
// source winner matches the bytes left by the legacy ascending materialization sequence.
static bool g536GuestOwnsTargetPage(const G261Res &r, uint32_t fbp,
                                    uint32_t pageRow, uint32_t pageCol)
{
    if (!g536PageOwnOn() || r.g536PageGenFbw == 0u ||
        r.g536PageGenFbw != r.fbw || r.fbw == 0u || r.fbw > 8u ||
        pageRow >= 16u || pageCol >= r.fbw || g_g144LastVram == nullptr)
        return false;
    const uint32_t p = pageRow * r.fbw + pageCol;
    if (p >= r.g536PageGen.size())
        return false;
    const uint32_t page = fbp + p;
    const uint64_t off = static_cast<uint64_t>(page) * 8192ull;
    if (page >= kG178PageCount || off + 8192ull > g_g261VramSize)
        return false;
    return g_g178PageGen[page].load(std::memory_order_relaxed) != r.g536PageGen[p];
}

// ===== G597 probe (behaviour-pure, DC2_G597_OWNCHK=1): WHY does the guard decline? =====
// G596 measured this guard firing on 0.28-0.38% of publications while `[G536:matchk]` reported guest
// VRAM moving under 38-80% of them. That is a gap of two orders of magnitude, and a guard's hit rate
// is a CONJUNCTION - so count every conjunct separately rather than guessing which one fails
// (G590's law, and G595's "two refuted guesses = stop"; this is guess ZERO).
static std::atomic<uint64_t> g_g597Calls{0}, g_g597BailOff{0}, g_g597BailNoSnap{0},
    g_g597BailFbwMismatch{0}, g_g597BailFbwRange{0}, g_g597BailVram{0}, g_g597BailRows{0},
    g_g597Scanned{0}, g_g597Moved{0}, g_g597Ran{0};
static bool g597OwnChkOn()
{
    static const bool on = envFlagEnabled("DC2_G597_OWNCHK");
    return on;
}
// The FIX. Default-OFF until the gate passes; then flipped, with DC2_G597_NO_SCOPEDANCHOR=1 as the
// single rollback. It only NARROWS what a re-anchor forgives, so it can only ever protect more
// pages than today — it cannot cause a publication to be skipped, only to preserve guest bytes.
static constexpr bool kG597PromotedDefaultOn = false;
static bool g597ScopedAnchorOn()
{
    static const bool on = !envFlagEnabled("DC2_G597_NO_SCOPEDANCHOR") &&
                           (kG597PromotedDefaultOn || envFlagEnabled("DC2_G597_SCOPEDANCHOR"));
    return on;
}
static void g597OwnReport()
{
    if (!g597OwnChkOn())
        return;
    const uint64_t n = g_g597Calls.load(std::memory_order_relaxed);
    if ((n % 2048u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G597:own] scoped=%d calls=%llu ran=%llu | bail(off=%llu noSnap=%llu "
                 "fbwMismatch=%llu fbwRange=%llu vram=%llu rows=%llu) | anchors=%llu "
                 "anchorPrePub=%llu mirrorAnchor=%llu mirrorScoped=%llu | pagesScanned=%llu "
                 "pagesMoved=%llu (%.3f%% of scanned)\n",
                 g597ScopedAnchorOn() ? 1 : 0,
                 (unsigned long long)n,
                 (unsigned long long)g_g597Ran.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailOff.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailNoSnap.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailFbwMismatch.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailFbwRange.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailVram.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597BailRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597AnchorTotal.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597AnchorPrePub.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597MirrorAnchor.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597MirrorScoped.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597Scanned.load(std::memory_order_relaxed),
                 (unsigned long long)g_g597Moved.load(std::memory_order_relaxed),
                 g_g597Scanned.load(std::memory_order_relaxed)
                     ? 100.0 * (double)g_g597Moved.load(std::memory_order_relaxed) /
                           (double)g_g597Scanned.load(std::memory_order_relaxed)
                     : 0.0);
}

// Collect every page inside the rows about to be published whose generation moved since the
// anchor. Fails CLOSED in the safe direction: if the snapshot is missing or the layout changed,
// nothing is protected and the legacy publication runs unchanged.
static void g536CollectGuestOwnedPages(const G261Res &r, uint32_t fbp, int rowLo, int rowHi,
                                       const uint8_t *vram, uint32_t vramSize,
                                       std::vector<G536PageSave> &out)
{
    out.clear();
    const bool g597 = g597OwnChkOn();
    if (g597)
    {
        g_g597Calls.fetch_add(1u, std::memory_order_relaxed);
        if (!g536PageOwnOn())
            g_g597BailOff.fetch_add(1u, std::memory_order_relaxed);
        else if (r.g536PageGenFbw == 0u)
            g_g597BailNoSnap.fetch_add(1u, std::memory_order_relaxed);
        else if (r.g536PageGenFbw != r.fbw)
            g_g597BailFbwMismatch.fetch_add(1u, std::memory_order_relaxed);
        else if (r.fbw == 0u || r.fbw > 8u)
            g_g597BailFbwRange.fetch_add(1u, std::memory_order_relaxed);
        else if (vram == nullptr)
            g_g597BailVram.fetch_add(1u, std::memory_order_relaxed);
        else if (rowHi < rowLo)
            g_g597BailRows.fetch_add(1u, std::memory_order_relaxed);
        else
            g_g597Ran.fetch_add(1u, std::memory_order_relaxed);
        g597OwnReport();
    }
    if (!g536PageOwnOn() || r.g536PageGenFbw == 0u || r.g536PageGenFbw != r.fbw ||
        r.fbw == 0u || r.fbw > 8u || vram == nullptr || rowHi < rowLo)
        return;
    const uint32_t basePage = fbp;
    const uint32_t pageRowLo = static_cast<uint32_t>(std::max(0, rowLo)) / 32u;
    const uint32_t pageRowHi = static_cast<uint32_t>(std::min(511, rowHi)) / 32u;
    for (uint32_t py = pageRowLo; py <= pageRowHi && py < 16u; ++py)
        for (uint32_t px = 0; px < r.fbw; ++px)
        {
            const uint32_t p = py * r.fbw + px;
            const uint32_t page = basePage + p;
            if (g597)
                g_g597Scanned.fetch_add(1u, std::memory_order_relaxed);
            if (!g536GuestOwnsTargetPage(r, fbp, py, px))
                continue; // FBO still owns this page
            if (g597)
                g_g597Moved.fetch_add(1u, std::memory_order_relaxed);
            const uint64_t off = static_cast<uint64_t>(page) * 8192ull;
            if (off + 8192ull > vramSize)
                continue;
            out.emplace_back();
            out.back().page = page;
            std::memcpy(out.back().bytes.data(), vram + off, 8192);
        }
}

static void g536RestoreGuestOwnedPages(const std::vector<G536PageSave> &saved, uint8_t *vram,
                                       uint32_t vramSize)
{
    for (const G536PageSave &s : saved)
    {
        const uint64_t off = static_cast<uint64_t>(s.page) * 8192ull;
        if (off + 8192ull <= vramSize)
            std::memcpy(vram + off, s.bytes.data(), 8192);
    }
    if (!saved.empty())
        g_g536PagesKept.fetch_add(saved.size(), std::memory_order_relaxed);
}

static void g536NoteMaterialize(const G261Res &r, uint32_t fbp, int cause)
{
    if (!g536MatChkOn())
        return;
    const uint64_t total = g_g536MatTotal.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t now = g261WindowGenNow(r, fbp);
    if ((total % 2048u) == 0u)
        std::fprintf(stderr, "[G536:matchk] total=%llu moved=%llu\n",
                     (unsigned long long)total,
                     (unsigned long long)g_g536MatMoved.load(std::memory_order_relaxed));
    if (now == r.genAtSkip)
        return;
    const uint64_t moved = g_g536MatMoved.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (moved <= 24u || (moved % 512u) == 0u)
        std::fprintf(stderr,
                     "[G536:matchk] VRAM MOVED UNDER PUBLICATION fbp=0x%x cause=%d "
                     "dirty=%d..%d cov=%d..%d win=%d..%d genAtSkip=%llu genNow=%llu "
                     "moved=%llu/%llu\n",
                     fbp, cause, r.dirtyLo, r.dirtyHi, r.covLo, r.covHi, r.winLo, r.winHi,
                     (unsigned long long)r.genAtSkip, (unsigned long long)now,
                     (unsigned long long)moved, (unsigned long long)total);
}

// G264 (default-on since G266): route guest re-uploads INTO the resident FBO instead of
// materializing around them (pillar-3 GPU-resident upload; G261 blocker 2). The upload hook
// defers eligible rects to `mirPending` rows; this flush reads those rows from VRAM (the guest
// upload has completed — the GIF stream is serial) and glTexSubImage2Ds them into the FBO,
// keeping the residency alive across the guest's per-frame scratch-page upload cycle.
static bool g264On()
{
    // G266 promotion: default-on (only consulted on wave paths, so the wave/stack kills gate it
    // upstream). Kill: DC2_G264_NO_UP_FBO=1. DC2_G264_UP_FBO=1 stays accepted; =0 disables.
    static const bool s_on = !envFlagEnabled("DC2_G264_NO_UP_FBO") &&
                             !envFlagDisabled("DC2_G264_UP_FBO");
    return s_on;
}

// Push pending mirror rows VRAM→FBO. Returns false on backend failure, in which case the FBO
// no longer matches guest intent for those rows — the CALLER must drop the residency WITHOUT a
// readback (a readback would clobber the newer VRAM upload bytes with stale FBO content).
static bool g264FlushMirror(int ti)
{
    G261Res &r = g_g261Res[ti];
    if (!r.mirPending)
        return true;
    std::unique_ptr<G327MirrorTimer> g327Timer;
    if (g327CensusOn())
        g327Timer = std::make_unique<G327MirrorTimer>();
    const int lo = r.mirLo, hi = r.mirHi;
    const uint32_t fbp = kG261Fbp[ti];
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange mirrorRange =
        (hi >= lo && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(std::max(0, lo)),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(hi - lo + 1))
            : G144BlockRange{};
    if (!g278FlushPendingDepthForRanges(
            1, mirrorRange, G144BlockRange{},
            hi >= lo && r.fbW > 0 && !mirrorRange.valid, false))
        return false; // deferred Z could not be published; do not decode stale VRAM into the FBO
    r.mirPending = false;
    r.mirLo = 512;
    r.mirHi = -1;
    uint8_t *vram = g_g144LastVram;
    auto clearBits = [&]() {
        if (hi >= lo)
            for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
                r.mirBits[static_cast<size_t>(y)].fill(0u);
    };
    if (vram == nullptr || hi < lo || lo < 0 || hi >= 512 ||
        r.fbW <= 0 || r.fbW > 512 || g_g261VramSize == 0u)
    {
        clearBits();
        return false;
    }
    static std::vector<uint32_t> s_g264Px; // single-threaded (flush-owner threads only)
    const bool g351MirDup = g351SplitOn() && fbp == 0x139u &&
                            (s_g351Band0IsB || s_g351Band1IsB);
    bool anyPatch = false;
    int patchLo = 512, patchHi = -1;
    uint32_t patchRects = 0u;
    for (int y = lo; y <= hi;)
    {
        const auto &rowMask = r.mirBits[static_cast<size_t>(y)];
        bool rowEmpty = true;
        for (uint64_t word : rowMask)
            rowEmpty = rowEmpty && (word == 0u);
        if (rowEmpty)
        {
            ++y;
            continue;
        }

        // Adjacent rows with the same target-pixel mask become one glTexSubImage2D rectangle
        // per horizontal run. This keeps the common full-page aliases to 1-2 queue jobs while
        // remaining exact for arbitrary CT32 swizzle aliases and disjoint pending uploads.
        int groupHi = y;
        while (groupHi < hi &&
               r.mirBits[static_cast<size_t>(groupHi + 1)] == rowMask)
            ++groupHi;
        const int rows = groupHi - y + 1;
        for (int x = 0; x < r.fbW;)
        {
            const auto bitSet = [&](int px) {
                return (rowMask[static_cast<size_t>(px >> 6)] &
                        (uint64_t{1} << (px & 63))) != 0u;
            };
            while (x < r.fbW && !bitSet(x))
                ++x;
            if (x >= r.fbW)
                break;
            const int x0 = x;
            while (x < r.fbW && bitSet(x))
                ++x;
            const int width = x - x0;
            const int glY = 511 - groupHi;
            const size_t pixelCount = static_cast<size_t>(width) * rows;
            s_g264Px.assign(pixelCount, 0u);
            bool packed = true;
            for (int gy = y; gy <= groupHi && packed; ++gy)
            {
                // GL input rows are bottom-first: highest GS y is the first buffer row.
                const size_t dstRow = static_cast<size_t>(groupHi - gy) * width;
                for (int gx = x0; gx < x0 + width; ++gx)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(
                        fbBlock, r.fbw, static_cast<uint32_t>(gx), static_cast<uint32_t>(gy));
                    if (off + 4u > g_g261VramSize)
                    {
                        packed = false;
                        break;
                    }
                    std::memcpy(&s_g264Px[dstRow + static_cast<size_t>(gx - x0)],
                                vram + off, 4);
                }
            }
            // G351: a mirror patch carries VRAM-authoritative bytes — under the surface split
            // the sibling FBO may be the one a later materialize reads, so patch it too
            // (writing newest-VRAM bytes into both FBOs is always coherent).
            if (!packed ||
                     !g264_backend_write_color_rect(fbp, r.fbW, 512, x0, glY,
                                                     width, rows, s_g264Px) ||
                     (g351MirDup &&
                      !g264_backend_write_color_rect(kG351Sprite139, r.fbW, 512, x0, glY,
                                                     width, rows, s_g264Px)))
            {
                clearBits();
                g_g264MirFail.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "[G264:mirror-fail] fbp=0x%x rect=%d,%d %dx%d fbW=%d packed=%u\n",
                             fbp, x0, y, width, rows, r.fbW, packed ? 1u : 0u);
                return false;
            }
            anyPatch = true;
            ++patchRects;
            patchLo = std::min(patchLo, y);
            patchHi = std::max(patchHi, groupHi);
        }
        y = groupHi + 1;
    }
    clearBits();
    if (!anyPatch)
        return false;
    if (patchLo < r.covLo) r.covLo = patchLo;
    if (patchHi > r.covHi) r.covHi = patchHi;
    // ===== G597 FIX: re-anchor the PER-PAGE baseline only for the rows this mirror PATCHED =====
    // The mirror reconciles guest VRAM into the FBO for `patchLo..patchHi` ONLY, so only those
    // pages' guest writes have been absorbed and only they may be forgiven. `g261UpdateWindow`
    // re-snapshots all 16*fbw pages, which additionally forgives every guest write elsewhere in
    // the claimed window — and this runs INSIDE g261Materialize, moments before
    // g536CollectGuestOwnedPages asks about exactly those pages. That is what makes G536's guard
    // read 0.28-0.38% against a 38-80% hazard rate.
    // `r.range` / `r.winLo` / `r.winHi` / `r.genAtSkip` are deliberately left EXACTLY as before
    // (one architectural variable per arm): only the per-page twin is scoped.
    if (g597OwnChkOn())
        g_g597MirrorAnchor.fetch_add(1u, std::memory_order_relaxed);
    if (g597ScopedAnchorOn() && r.g536PageGenFbw == r.fbw && r.fbw != 0u && r.fbw <= 8u)
    {
        const std::array<uint64_t, 16 * 8> savedGens = r.g536PageGen;
        const uint32_t savedFbw = r.g536PageGenFbw;
        g261UpdateWindow(ti);
        if (r.g536PageGenFbw == savedFbw && r.g536PageGenFbw == r.fbw)
        {
            const uint32_t prLo = static_cast<uint32_t>(std::max(0, patchLo)) / 32u;
            const uint32_t prHi = static_cast<uint32_t>(std::min(511, patchHi)) / 32u;
            for (uint32_t py = 0; py < 16u; ++py)
            {
                if (py >= prLo && py <= prHi)
                    continue; // genuinely mirrored: the FBO absorbed these guest bytes
                for (uint32_t px = 0; px < r.fbw; ++px)
                {
                    const uint32_t p = py * r.fbw + px;
                    if (p < r.g536PageGen.size())
                        r.g536PageGen[p] = savedGens[p]; // NOT mirrored: keep the older anchor
                }
            }
            if (g597OwnChkOn())
                g_g597MirrorScoped.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    else
    {
        g261UpdateWindow(ti); // re-anchor claimed rows + gen baseline past the upload's own bumps
    }
    // FBO == VRAM again for the mirrored rows; keep the snapshot coherent if one is live.
    G178FbSnap &snap = g_g178FbSnap[fbp];
    if (snap.valid)
    {
        if (lo < snap.rowLo) snap.rowLo = lo;
        if (hi > snap.rowHi) snap.rowHi = hi;
        snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                     static_cast<uint32_t>(r.fbW), 512u);
    }
    if (g262CensusOn())
    {
        static std::atomic<uint32_t> s_g264PatchLog{0};
        if (s_g264PatchLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr, "[G264:patch] fbp=0x%x rows=%d..%d rects=%u fbW=%d\n",
                         fbp, patchLo, patchHi, patchRects, r.fbW);
    }
    g_g264MirFlushed.fetch_add(1u, std::memory_order_relaxed);
    // G280: the mirror wrote VRAM-authoritative bytes into these FBO rows — re-sync + drop any
    // alias-overlay tiles they touch and stale every overlay copied FROM this target.
    g280NoteMirrorPatch(ti, patchLo, patchHi);
    g310NoteProducerWrite(ti, patchLo, patchHi);
    return true;
}

// G264: flush every pending upload mirror before a GPU flush touches classify/invariant/bind
// state. A mirror failure means the FBO is stale for rows VRAM now owns — drop that residency
// WITHOUT a readback (loud, bounded; the fresh VRAM upload bytes must win).
static void g264FlushMirrorsAtFlush()
{
    if (!g264On())
        return;
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
    {
        G261Res &r = g_g261Res[i];
        if (!r.mirPending)
            continue;
        if (!g264FlushMirror(static_cast<int>(i)))
        {
            g280DropOverlays(static_cast<int>(i)); // residency dies; nothing publishes them
            r.dirty = false;
            r.dirtyLo = 512;
            r.dirtyHi = -1;
            g261UpdateWindow(static_cast<int>(i));
            G178FbSnap &snap = g_g178FbSnap[kG261Fbp[i]];
            snap.valid = false;
            snap.genSum = 0;
            snap.rowLo = 1 << 30;
            snap.rowHi = -1;
            g310NoteTargetClean(false);
        }
    }
}

// Read the dirty row window back from the target's FBO into guest VRAM, advance the page write
// generations (this is when VRAM bytes actually change), and refresh the fb snapshot so the next
// GPU flush of this target does not re-upload bytes the FBO already equals. On ANY failure the
// residency is dropped loudly with VRAM left as-is (bounded stale-pixel glitch, never a hang or
// a silent overwrite of newer guest bytes).
// G305 forward decls (definitions live next to g178TryFlushGpu). g261Materialize is a target-FBO
// read consumer, so it must fence any in-flight async packet first.
static bool g305AsyncOn();
static bool g305DrainRequired();
static void g305DrainPending();

// ===== G432: the SECOND colour-readback unpack site =====
// G418 replaced the serial per-pixel `addrPSMCT32` + 4-byte-memcpy publication loop with an exact
// one-page offset table + paired 8-byte stores (+ optional row lanes) and promoted it — but only at
// the G144 batch publication site in rasterizer_vram_materialization.inc. `g261Materialize` runs the
// IDENTICAL loop shape on the resident-RTT publication path, and the G432 census measured that path
// as the largest single block on the GS front thread (the local->local TRXDIR edge alone spends
// ~5.3 ms/f in g261MaterializeForRanges and ~2.3 ms/f in g285PrepareL2lConsume, both of which end
// here). This part forwards that site to the same proven helper.
//
// EXACTNESS. The legacy loop indexes its source as `srcRow = (511 - y - glY) * fbW` with
// `glY = 512 - 1 - dirtyHi`, i.e. `(dirtyHi - y) * fbW`. g418UnpackColorRows computes
// `(fbH - 1 - y) * fbW`, so passing `fbH = dirtyHi + 1` reproduces the same index for every y in
// [dirtyLo, dirtyHi]. Everything else (address table, paired stores, clipping fallback) is the
// G418 mechanism unchanged, already oracle-verified at 109,051,904 pixels with bad=0.
//
// Declared here because g418_readback_unpack.inc is included later in this TU.
// G498 added `lanesOverride` (0 = G418's own promoted lane policy). The default argument lives on
// THIS declaration, the first one in the TU — repeating it on the definition would make the two
// signatures a two-member overload set and every 10-argument call ambiguous.
static void g418UnpackColorRows(uint8_t *vram, uint32_t vramSize,
                                const std::vector<uint32_t> &src,
                                uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                                int rowLo, int rowHi, bool legacy, int lanesOverride = 0);

// DC2_G432_NO_MAT_UNPACK=1 restores the serial per-pixel publication loop (rollback arm).
static bool g432MatUnpackOn()
{
    static const bool on = !envFlagEnabled("DC2_G432_NO_MAT_UNPACK");
    return on;
}

// Sub-millisecond lever ⇒ the within-process randomized A/B (DC2_G419_AB=matunpack) is its only
// admissible gate. g419_ab_instrument.inc is compiled later in this TU, so both the selector and its
// lever id are referenced by forward declaration / by value.
static constexpr int kG432MatUnpackLeverId = 10; // == kG419Lever::kG432LeverMatUnpack
static bool g419AbArmFor(int lever, bool defaultOn);

// G454 dependency-boundary deletion probes. IDs mirror G419Lever, declared later in this TU.
// Candidate output is knowingly stale; downstream publication work remains unchanged so the
// delta prices the synchronous readback edge, not a smaller pixel-copy microbenchmark.
static constexpr int kG454RbAllLeverId = 20;
static constexpr int kG454RbL2lLeverId = 21;
static constexpr int kG454RbResyncLeverId = 22;
static constexpr int kG454RbDisplayLeverId = 23;
static bool g454SkipReadbackAt(int siteLever)
{
    if (g419AbArmFor(kG454RbAllLeverId, false))
        return true;
    return siteLever != 0 && g419AbArmFor(siteLever, false);
}

// Behaviour-pure timing of this publication loop only (DC2_G432_CENSUS=1).
static bool g432MatCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G432_CENSUS");
    return on;
}
static uint64_t g_g432MatUnpackNs = 0, g_g432MatUnpackN = 0, g_g432MatUnpackPx = 0;

// ===== G599: THE WRITE — what does a publication actually CHANGE in guest VRAM? (DEFAULT OFF) ====
//
// G598 settled that the MAP-15 sky defect is entirely in what the port leaves in guest VRAM at the
// sky's texture base (`0x2720 = 0x139 << 5`) — the guest packet stream is byte-identical in both
// arms — and it REFUTED the read side: at the sky read the target reads `dirty=0 anyDirty=0`, so no
// admission change can serve those texels from a live FBO. The wrong bytes are already committed
// when the read happens. What is left is the WRITE, and `g261Materialize` is the one write G598
// never excluded (`DC2_G528_NO_PUB=1` deletes the FLUSH publication, not this readback).
//
// So measure the write instead of arguing about it. Per publication this records how many BYTES of
// each of the target's OWN 8 KB pages the unpack actually changed. A publication that changes zero
// bytes in a page is harmless there; one that changes bytes in the page a texture consumer reads is
// the edge under investigation. Paired with `[G599:who]` (the consumer side, in
// rasterizer_vram_materialization.inc) it closes the loop: this end says "the publication rewrote
// page 0x139", that end says "the sky read observed page 0x139 last written by <source>".
//
//   DC2_G599_PUB=1            arm the census
//   DC2_G599_SF_LO/_SF_HI     window it on the SCRIPT clock (defect window: sf 1958..2440)
//   DC2_G599_EVERY=<n>        print one aggregate line every n publications (default 256)
//
// Behaviour-pure: every byte compared and every counter touched is inside `if (g599)`. The page
// walk is g536CollectGuestOwnedPages' walk verbatim, so the two describe the same page set.
static bool g599PubOn()
{
    static const bool on = envFlagEnabled("DC2_G599_PUB");
    return on;
}
static uint32_t g599EnvU32(const char *name, uint32_t dflt)
{
    const char *e = std::getenv(name);
    if (e == nullptr)
        return dflt;
    const long v = std::strtol(e, nullptr, 0);
    return v > 0 ? static_cast<uint32_t>(v) : dflt;
}
// ⚠️ The script clock is a GUEST-frame quantity, so it only moves between flushes. That is exactly
// the resolution this census needs (the defect window is ~217 guest frames wide) and no finer.
static bool g599InWindow()
{
    static const uint32_t lo = g599EnvU32("DC2_G599_SF_LO", 0u);
    static const uint32_t hi = g599EnvU32("DC2_G599_SF_HI", 0u);
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    return sf >= lo && (hi == 0u || sf <= hi);
}
// ===== G601 (default OFF): the `s05` hair-band flicker instruments =====
// Measured: on every 4th guest frame ALL 51 sprites of the `0x13a` glow composite (tbp=0x2740,
// CT32, tbw=8, bilinear FST sprites) decode guest VRAM instead of binding the producer FBO, and
// guest VRAM is BLACK there — the band vanishes for one frame. `[G599:bind]` names the failing
// conjunct as `dirty=0`, and the transition 1 -> 0 happens INSIDE the first entry's own per-target
// pass, i.e. that entry's bind was refused and its fallback materialized the target. These two
// probes answer the two remaining questions in one run:
//   DC2_G601_REJ=1  `[G601:rej]` — WHY g263BilinearSpriteBind refused a sprite, uncapped and
//                   windowed on the script clock (the shipped `[G263:rej]` caps at 16 lines from
//                   process start, which this route consumes ~100 s before the window opens).
//   DC2_G601_MAT=1  `[G601:mat]` — every g261Materialize of the traced target, with the mean of the
//                   band rectangle IN THE FBO BUFFER THAT IS ABOUT TO BE PUBLISHED. A black FBO and
//                   a black publication are different bugs and this separates them.
//   DC2_G601_FBP=<hex>  target to trace (default 0x13a) · DC2_G601_SF_LO/_HI (falls back to G599's)
static bool g601RejOn()
{
    static const bool on = envFlagEnabled("DC2_G601_REJ");
    return on;
}
static bool g601MatOn()
{
    static const bool on = envFlagEnabled("DC2_G601_MAT");
    return on;
}
static uint32_t g601Fbp()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G601_FBP");
        return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0x13au;
    }();
    return v;
}
static bool g601Window()
{
    static const uint32_t lo = g599EnvU32("DC2_G601_SF_LO", g599EnvU32("DC2_G599_SF_LO", 0u));
    static const uint32_t hi = g599EnvU32("DC2_G601_SF_HI", g599EnvU32("DC2_G599_SF_HI", 0u));
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    return sf >= lo && (hi == 0u || sf <= hi);
}
// ===== G599 CEILING PROBE (default OFF, knowingly incorrect): delete ONE publication's write =====
// `DC2_G400_NO_RESIDENT_139=1` fixes the sky but also halves the frame rate and removes the whole
// GPU path, so it cannot say WHICH of residency's effects matters. This probe deletes exactly one:
// the guest-VRAM WRITE of a chosen target's publication. Residency, the readback, the snapshots and
// every other consumer edge are untouched, so the arm runs at the CONTROL'S OWN SPEED — the property
// G598 §3 had to buy with a sibling-target lever.
//
//   DC2_G599_NOPUB=<fbp hex>     e.g. 139 — that target's publications leave guest VRAM unchanged
//   DC2_G599_NOPUB_PLO/_PHI      restrict the restore to page offsets [plo,phi] of the target
//                                (p = page - fbp), so the write can be bisected page by page
//   DC2_G599_NOPUB_FBW=<n>       restrict it to publications at that FBW (0x139 publishes at both
//                                8 and 2; G568's law says never treat the two as one population)
static uint32_t g599NoPubFbp()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G599_NOPUB");
        return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0xFFFFFFFFu;
    }();
    return v;
}
static bool g599NoPubFor(uint32_t fbp, uint32_t fbw)
{
    if (g599NoPubFbp() != fbp)
        return false;
    static const uint32_t only = g599EnvU32("DC2_G599_NOPUB_FBW", 0u);
    return only == 0u || only == fbw;
}
static std::atomic<uint64_t> g_g599NoPubHits{0}, g_g599NoPubPages{0};

struct G599PubBucket
{
    uint64_t calls = 0u, pages = 0u, pagesChanged = 0u, bytesChanged = 0u;
    uint64_t baseSeen = 0u, baseChanged = 0u, baseBytes = 0u;
};
// [target][fbw]: fbw is part of the publication's identity (0x139 is written at both 8 and 2), and
// G568's law says never to collapse a bimodal population into one average.
static G599PubBucket g_g599Pub[kG248TargetCount][9];
static uint64_t g_g599PubCalls = 0u, g_g599PubPrints = 0u;
static void g599PubReport(); // per-bucket summary; emitted from the aggregate print below

// Flush-owner thread only (same ownership as s_g261Px / s_g536Saved).
static std::vector<uint32_t> s_g599Pages;
static std::vector<uint8_t> s_g599Before;

// Snapshot every target-local page the publication is about to write. Returns the page count.
static size_t g599SnapshotPages(const G261Res &r, uint32_t fbp, int rowLo, int rowHi,
                                const uint8_t *vram, uint32_t vramSize)
{
    s_g599Pages.clear();
    if (vram == nullptr || rowHi < rowLo || r.fbw == 0u || r.fbw > 8u)
        return 0u;
    const uint32_t pageRowLo = static_cast<uint32_t>(std::max(0, rowLo)) / 32u;
    const uint32_t pageRowHi = static_cast<uint32_t>(std::min(511, rowHi)) / 32u;
    for (uint32_t py = pageRowLo; py <= pageRowHi && py < 16u; ++py)
        for (uint32_t px = 0; px < r.fbw; ++px)
        {
            const uint32_t page = fbp + py * r.fbw + px;
            if (static_cast<uint64_t>(page) * 8192ull + 8192ull > vramSize)
                continue;
            s_g599Pages.push_back(page);
        }
    s_g599Before.resize(s_g599Pages.size() * 8192u);
    for (size_t i = 0; i < s_g599Pages.size(); ++i)
        std::memcpy(s_g599Before.data() + i * 8192u,
                    vram + static_cast<uint64_t>(s_g599Pages[i]) * 8192ull, 8192);
    return s_g599Pages.size();
}

// Compare, accumulate and report. `basePage` is the target's page 0 — the block a texture consumer
// binds when it samples the target's base (`tbp0 == fbp << 5`), i.e. the sky's page for 0x139.
static void g599ReportPublish(int ti, uint32_t fbp, int cause, const G261Res &r,
                              int rowLo, int rowHi, const uint8_t *vram, size_t nPages,
                              size_t guarded)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return; // kG248TargetCount is 14, not a power of two — bound, never mask
    G599PubBucket &b = g_g599Pub[ti][r.fbw <= 8u ? r.fbw : 0u];
    ++b.calls;
    b.pages += nPages;
    uint64_t changedPages = 0u, changedBytes = 0u, baseBytes = 0u;
    uint64_t baseHashBefore = 0u, baseHashAfter = 0u;
    bool baseSeen = false;
    for (size_t i = 0; i < nPages; ++i)
    {
        const uint8_t *before = s_g599Before.data() + i * 8192u;
        const uint8_t *after = vram + static_cast<uint64_t>(s_g599Pages[i]) * 8192ull;
        uint64_t diff = 0u;
        for (uint32_t k = 0; k < 8192u; ++k)
            diff += (before[k] != after[k]) ? 1u : 0u;
        if (diff != 0u)
            ++changedPages;
        changedBytes += diff;
        if (s_g599Pages[i] == fbp)
        {
            baseSeen = true;
            baseBytes = diff;
            // Same FNV-1a fingerprint [G599:who] prints at the consumer, so a publication's
            // OUTPUT hash can be matched against the hash the sky read later observes. A
            // changed-byte COUNT says a page moved; the pair of hashes says WHICH image it holds.
            baseHashBefore = 1469598103934665603ull;
            baseHashAfter = 1469598103934665603ull;
            for (uint32_t k = 0; k < 8192u; ++k)
            {
                baseHashBefore = (baseHashBefore ^ before[k]) * 1099511628211ull;
                baseHashAfter = (baseHashAfter ^ after[k]) * 1099511628211ull;
            }
        }
    }
    b.pagesChanged += changedPages;
    b.bytesChanged += changedBytes;
    if (baseSeen)
    {
        ++b.baseSeen;
        b.baseBytes += baseBytes;
        if (baseBytes != 0u)
            ++b.baseChanged;
    }
    static const uint32_t every = g599EnvU32("DC2_G599_EVERY", 256u);
    const uint64_t n = ++g_g599PubCalls;
    // Always print the first 24 publications that rewrite the BASE page — that is the event the
    // phase is hunting, and it is far too rare to survive an "every n" filter.
    const bool baseEvent = baseSeen && baseBytes != 0u && g_g599PubPrints < 24u;
    if (baseEvent)
        ++g_g599PubPrints;
    if (!baseEvent && (n % every) != 0u)
        return;
    std::fprintf(stderr,
                 "[G599:pub] %s sf=%u n=%llu fbp=0x%x cause=%d fbw=%u fbW=%d dirty=%d..%d "
                 "cov=%d..%d win=%d..%d snapFbw=%u | pages=%zu chgPages=%llu chgBytes=%llu "
                 "guarded=%zu | base(page=0x%x seen=%u chgBytes=%llu %016llx->%016llx)\n",
                 baseEvent ? "BASEWRITE" : "agg",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(n), fbp, cause, r.fbw, r.fbW, rowLo, rowHi,
                 r.covLo, r.covHi, r.winLo, r.winHi, r.g536PageGenFbw, nPages,
                 static_cast<unsigned long long>(changedPages),
                 static_cast<unsigned long long>(changedBytes), guarded, fbp,
                 baseSeen ? 1u : 0u, static_cast<unsigned long long>(baseBytes),
                 static_cast<unsigned long long>(baseHashBefore),
                 static_cast<unsigned long long>(baseHashAfter));
    if (!baseEvent && (n % (static_cast<uint64_t>(every) * 16u)) == 0u)
        g599PubReport();
}

// Put the snapshotted bytes back, so the publication's net effect on guest VRAM is nothing. Runs
// AFTER g536RestoreGuestOwnedPages, so it subsumes the guard rather than racing it.
static void g599UndoPublish(uint32_t fbp, uint8_t *vram, size_t nPages)
{
    static const uint32_t plo = g599EnvU32("DC2_G599_NOPUB_PLO", 0u);
    static const uint32_t phi = g599EnvU32("DC2_G599_NOPUB_PHI", 0xFFFFu);
    uint64_t restored = 0u;
    for (size_t i = 0; i < nPages; ++i)
    {
        const uint32_t p = s_g599Pages[i] - fbp;
        if (p < plo || p > phi)
            continue;
        std::memcpy(vram + static_cast<uint64_t>(s_g599Pages[i]) * 8192ull,
                    s_g599Before.data() + i * 8192u, 8192);
        ++restored;
    }
    const uint64_t n = g_g599NoPubHits.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g599NoPubPages.fetch_add(restored, std::memory_order_relaxed);
    if (n <= 4u || (n % 4096u) == 0u)
        std::fprintf(stderr,
                     "[G599:nopub] CEILING PROBE fbp=0x%x n=%llu pagesRestored=%llu "
                     "(this call %llu of %zu, window p=%u..%u)\n",
                     fbp, (unsigned long long)n,
                     (unsigned long long)g_g599NoPubPages.load(std::memory_order_relaxed),
                     (unsigned long long)restored, nPages, plo, phi);
}

static void g599PubReport()
{
    if (!g599PubOn())
        return;
    for (int t = 0; t < kG248TargetCount; ++t)
        for (uint32_t w = 0; w < 9u; ++w)
        {
            const G599PubBucket &b = g_g599Pub[t][w];
            if (b.calls == 0u)
                continue;
            std::fprintf(stderr,
                         "[G599:pubsum] fbp=0x%x fbw=%u calls=%llu pages=%llu chgPages=%llu "
                         "(%.1f%%) chgBytes=%llu | base seen=%llu changed=%llu (%.1f%%) "
                         "bytes/seen=%.0f\n",
                         kG261Fbp[t], w, (unsigned long long)b.calls,
                         (unsigned long long)b.pages, (unsigned long long)b.pagesChanged,
                         b.pages ? 100.0 * (double)b.pagesChanged / (double)b.pages : 0.0,
                         (unsigned long long)b.bytesChanged, (unsigned long long)b.baseSeen,
                         (unsigned long long)b.baseChanged,
                         b.baseSeen ? 100.0 * (double)b.baseChanged / (double)b.baseSeen : 0.0,
                         b.baseSeen ? (double)b.baseBytes / (double)b.baseSeen : 0.0);
        }
}

static void g261Materialize(int ti, int cause)
{
    g324NoteObserver(1u); // direct resident-FBO/guest-authority consumer
    if (g305DrainRequired())
        g305DrainPending();
    G261Res &r = g_g261Res[ti];
    if (!r.dirty)
    {
        // Not dirty but a mirror may still be pending (e.g. noted just before an invariant
        // drop): flush it so the FBO does not keep stale rows for a later wave.
        if (r.mirPending)
            g264FlushMirror(ti);
        return;
    }
    // G636: the mirror of the FBO-seed hazard. This publication writes the FBO back OVER guest
    // VRAM; a G630 publication of the same pages afterwards would then overwrite it with the OLDER
    // raw content, because G630's authority is settled lazily and this write does not clear it.
    // `g261MaterializeForRanges` already orders the two correctly, but a DIRECT `g261Materialize`
    // (g261PrepareCpuReplay, g261MaterializeAllDirty, the T8 atlas builders, kG261MatResync) does
    // not — so the ordering belongs here, where every caller gets it.
    if (!s_g636NoMatPub && g630DomainOn() && g630AnyDirty() && r.fbW > 0 &&
        r.dirtyHi >= r.dirtyLo)
    {
        const G144BlockRange g636Pub = g144RangeForRect(
            framePageBaseToBlock(kG261Fbp[ti]), GS_PSM_CT32, r.fbw, 0u,
            static_cast<uint32_t>(std::max(0, r.dirtyLo)),
            static_cast<uint32_t>(r.fbW),
            static_cast<uint32_t>(r.dirtyHi - r.dirtyLo + 1));
        g630MaterializeForRanges(G144BlockRange{}, g636Pub, false, !g636Pub.valid, cause);
        if (s_g636Stat) g_g636MatPub.fetch_add(1u, std::memory_order_relaxed);
    }
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    const uint32_t fbp = kG261Fbp[ti];
    // G597: everything from here to g536CollectGuestOwnedPages is "after the hazard, before the
    // guard" — a re-anchor landing in this span destroys the evidence the guard reads.
    t_g597InMatPrePub = true;
    g536NoteMaterialize(r, fbp, cause); // G536 probe: has guest VRAM moved since the GPU render?
    const int rows = r.dirtyHi - r.dirtyLo + 1;
#if defined(PS2X_G662_DIAG)
    // G662 P6: the roadmap asks for "per-row Local publication work" to be replaced by batched
    // transfers. Within ONE call there is nothing left to batch (one read_color over the whole
    // span, one g418UnpackColorRows, one g178BumpRectImpl), so the remaining question is whether
    // consecutive publications could have been one. Booked here, keyed to the row span.
    if (cause == kG261MatLocal)
        g662NoteMatLocal(fbp, r.dirtyLo, r.dirtyHi);
    // G662 [G662:colspan]: this publication is about to transfer `rows * r.fbW` pixels. Price that
    // against the column extent its producers actually covered — every cause, because the readback
    // is the same full-width call on all of them.
    g662NoteColSpan(fbp, r.fbW, r.dirtyColLo, r.dirtyColHi, rows);
#endif
    // G590: this is the edge where GPU authority is surrendered back to guest VRAM. A GPU-resident
    // model has to delete it, so count it by TARGET x CAUSE x ROWS — the two existing counters
    // (g_g261MatByTarget per target, g_g310MatN per cause) cannot be crossed after the fact.
    g590NoteMaterialize(fbp, cause, rows);
    if (g345StatOn() && g345FamilyFbp(fbp) && rows > 0)
        g345NoteFamReadbackFbp(fbp, static_cast<uint64_t>(rows));
    // G351: attribute this 0x139 FBO→VRAM publication (and the consumer demand behind its cause)
    // to the surface that last wrote the wide FBO — the split's consumer→surface map.
    if (fbp == 0x139u && rows > 0)
        g351NoteCon(kG351SiteMat, cause, static_cast<uint64_t>(rows));
    if (g310StatOn())
    {
        const int pageLo = std::max(0, r.dirtyLo) / 32;
        const int pageHi = std::min(511, r.dirtyHi) / 32;
        const uint64_t pages =
            pageHi >= pageLo ? static_cast<uint64_t>(pageHi - pageLo + 1) * r.fbw : 0u;
        g_g310MatN[cause & 7].fetch_add(1u, std::memory_order_relaxed);
        g_g310MatPages[cause & 7].fetch_add(pages, std::memory_order_relaxed);
    }
    uint8_t *vram = g_g144LastVram;
    bool ok = false;
    const uint32_t fbBlock = framePageBaseToBlock(fbp);
    const G144BlockRange dirtyRange =
        (rows > 0 && r.fbW > 0)
            ? g144RangeForRect(fbBlock, GS_PSM_CT32, r.fbw, 0u,
                               static_cast<uint32_t>(r.dirtyLo),
                               static_cast<uint32_t>(r.fbW),
                               static_cast<uint32_t>(rows))
            : G144BlockRange{};
    const bool depthPublished = g278FlushPendingDepthForRanges(
        1, G144BlockRange{}, dirtyRange, false,
        rows > 0 && r.fbW > 0 && !dirtyRange.valid);
    static std::vector<uint32_t> s_g261Px; // single-threaded (flush-owner threads only)
    // G264: bring the FBO current with any pending upload mirror FIRST — the readback below
    // must not clobber newer VRAM upload bytes with pre-upload FBO content. Mirror failure ⇒
    // ok stays false ⇒ the loud drop path below runs with VRAM untouched (upload preserved).
    // G280: alias-overlay tiles are re-synced from VRAM (and dropped) BEFORE the readback so
    // this publication never writes alias-page bytes the dst view does not own; a restore
    // failure takes the same loud no-readback drop path as a mirror failure.
    if (depthPublished && g280PrepareMaterialize(ti) && g264FlushMirror(ti) &&
        vram != nullptr && rows > 0 && r.fbW > 0 && g_g261VramSize != 0u)
    {
        const int glY = 512 - 1 - r.dirtyHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const int g455Site = fbp == 0x139u && cause == kG261MatLocal ? kG455Local139
                           : fbp == 0x139u && cause == kG261MatResync ? kG455Resync139
                                                                          : -1;
        const G455Ticket g455Ticket = g455BeginRead(
            g455Site, s_g455ResWriteSeq, fbp, r.fbw, r.fbW, r.dirtyLo, r.dirtyHi);
        // G351: under the surface split the CURRENT wide-0x139 content lives in the per-band
        // routed FBO (scene rows on the fbp key, sprite-only rows possibly on the sibling).
        const bool g351Split139 = g351SplitOn() && fbp == 0x139u;
        const uint32_t g351K0 = g351Split139 ? g351Band0Key139() : fbp; // rows <= 415
        const uint32_t g351K1 = g351Split139 ? g351Band1Key139() : fbp; // rows >= 416
        bool readOk;
        uint32_t g351ReadKey = fbp; // 0 = mixed-band read (no single FBO equals VRAM after)
        const int g454Site = cause == kG261MatLocal ? kG454RbL2lLeverId
                            : cause == kG261MatResync ? kG454RbResyncLeverId
                                                     : 0;
        const bool g454Skip = g454SkipReadbackAt(g454Site);
        if (g454Skip)
        {
            s_g261Px.resize(static_cast<size_t>(r.fbW) * rows);
            readOk = true;
        }
        else if (!g351Split139 || g351K0 == g351K1 || r.dirtyLo >= 416 || r.dirtyHi <= 415)
        {
            const uint32_t k = r.dirtyLo >= 416 ? g351K1 : g351K0;
            g351ReadKey = k;
            readOk = g178_backend_read_color(k, r.fbW, 512, glY, rows, s_g261Px);
        }
        else
        {
            // Two-part read: the dirty span crosses the band boundary and the bands live in
            // different FBOs. GL rows are bottom-first, so band-1 (higher GS rows) fills the
            // buffer head and band-0 the tail — same layout the copy loop below expects.
            ++s_g351MixedMat;
            g351ReadKey = 0u;
            static std::vector<uint32_t> s_g351Hi, s_g351Lo; // flush-owner thread only
            const int rows1 = r.dirtyHi - 416 + 1, rows0 = 415 - r.dirtyLo + 1;
            readOk = g178_backend_read_color(g351K1, r.fbW, 512, glY, rows1, s_g351Hi) &&
                     g178_backend_read_color(g351K0, r.fbW, 512, 512 - 1 - 415, rows0,
                                             s_g351Lo) &&
                     s_g351Hi.size() == static_cast<size_t>(r.fbW) * rows1 &&
                     s_g351Lo.size() == static_cast<size_t>(r.fbW) * rows0;
            if (readOk)
            {
                s_g261Px.resize(static_cast<size_t>(r.fbW) * rows);
                std::memcpy(s_g261Px.data(), s_g351Hi.data(),
                            s_g351Hi.size() * sizeof(uint32_t));
                std::memcpy(s_g261Px.data() + s_g351Hi.size(), s_g351Lo.data(),
                            s_g351Lo.size() * sizeof(uint32_t));
            }
        }
        g455EndRead(g455Ticket);
        if (g279Profile)
            g_g279MatReadNs[cause & 7].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g261Px.size() == static_cast<size_t>(r.fbW) * rows;
        if (ok)
        {
            // G432: exact G418 unpack (table + paired stores) at this second publication site.
            // `fbH = r.dirtyHi + 1` reproduces the legacy `(511 - y - glY)` source row index.
            const bool g432MatCensus = g432MatCensusOn();
            const auto g432T0 = g432MatCensus ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
            const bool g432Fast =
                g419AbArmFor(kG432MatUnpackLeverId, g432MatUnpackOn());
            // G536: save any page inside these rows that the guest has written since this
            // residency anchored, so the full-width publication below cannot destroy it.
            static std::vector<G536PageSave> s_g536Saved; // flush-owner thread only
            g536CollectGuestOwnedPages(r, fbp, r.dirtyLo, r.dirtyHi, vram, g_g261VramSize,
                                       s_g536Saved);
            t_g597InMatPrePub = false; // G597: the guard has read the anchor
            g_g536PubTotal.fetch_add(1u, std::memory_order_relaxed);
            if (!s_g536Saved.empty())
                g_g536PubGuarded.fetch_add(1u, std::memory_order_relaxed);
            // G599 (default OFF): snapshot the target's own pages so the publication's ACTUAL
            // byte effect can be measured, not inferred. Straddles the guard's restore so what it
            // reports is the net effect on guest VRAM, which is what a consumer reads.
            const bool g599 = g599PubOn() && g599InWindow();
            const bool g599NoPub = g599NoPubFor(fbp, r.fbw);
            const size_t g599N = (g599 || g599NoPub)
                                     ? g599SnapshotPages(r, fbp, r.dirtyLo, r.dirtyHi, vram,
                                                         g_g261VramSize)
                                     : 0u;
            g418UnpackColorRows(vram, g_g261VramSize, s_g261Px, fbBlock, r.fbw, r.fbW,
                                r.dirtyHi + 1, r.dirtyLo, r.dirtyHi,
                                /*legacy=*/!g432Fast);
            g536RestoreGuestOwnedPages(s_g536Saved, vram, g_g261VramSize);
            if (g601MatOn() && fbp == g601Fbp() && g601Window())
            {
                // Mean of the hair-band rectangle in the buffer just read back from the FBO.
                // Layout: `rows` rows of r.fbW, GL bottom-first, so buffer row k = GS row
                // (r.dirtyHi - k) — the same mapping g418UnpackColorRows was handed above.
                double sum = 0.0;
                uint64_t nPx = 0u;
                for (int y = 140; y <= 200; ++y)
                {
                    if (y < r.dirtyLo || y > r.dirtyHi)
                        continue;
                    const size_t row = static_cast<size_t>(r.dirtyHi - y);
                    for (int x = 420; x <= 485 && x < r.fbW; ++x)
                    {
                        const size_t o = row * static_cast<size_t>(r.fbW) +
                                         static_cast<size_t>(x);
                        if (o >= s_g261Px.size())
                            continue;
                        const uint32_t c = s_g261Px[o];
                        sum += static_cast<double>((c & 0xFFu) + ((c >> 8) & 0xFFu) +
                                                   ((c >> 16) & 0xFFu));
                        ++nPx;
                    }
                }
                std::fprintf(stderr,
                             "[G601:mat] sf=%u fbp=0x%x cause=%d rows=%d..%d fbW=%d cov=%d..%d "
                             "guarded=%zu bandFboMean=%.2f n=%llu\n",
                             g_dc2ScriptFrame.load(std::memory_order_relaxed), fbp, cause,
                             r.dirtyLo, r.dirtyHi, r.fbW, r.covLo, r.covHi, s_g536Saved.size(),
                             nPx ? sum / static_cast<double>(nPx) / 3.0 : -1.0,
                             static_cast<unsigned long long>(nPx));
            }
            if (g599)
                g599ReportPublish(ti, fbp, cause, r, r.dirtyLo, r.dirtyHi, vram, g599N,
                                  s_g536Saved.size());
            if (g599NoPub)
                g599UndoPublish(fbp, vram, g599N); // CEILING PROBE — knowingly incorrect
            if (g432MatCensus)
            {
                g_g432MatUnpackNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g432T0).count());
                ++g_g432MatUnpackN;
                g_g432MatUnpackPx += static_cast<uint64_t>(rows) *
                                     static_cast<uint64_t>(r.fbW);
                if ((g_g432MatUnpackN % 2000u) == 0u)
                    std::fprintf(stderr,
                                 "[G432:matunpack] fast=%d n=%llu ms=%.1f Mpx=%.1f nsPerPx=%.2f\n",
                                 g432MatUnpackOn() ? 1 : 0,
                                 static_cast<unsigned long long>(g_g432MatUnpackN),
                                 static_cast<double>(g_g432MatUnpackNs) / 1.0e6,
                                 static_cast<double>(g_g432MatUnpackPx) / 1.0e6,
                                 g_g432MatUnpackPx
                                     ? static_cast<double>(g_g432MatUnpackNs) /
                                           static_cast<double>(g_g432MatUnpackPx)
                                     : 0.0);
            }
            g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW), static_cast<uint32_t>(rows),
                             1u | (static_cast<uint32_t>(cause & 0xff) << 8u));
            // G570 trace source: low byte 1 = resident RTT FBO publication; high byte =
            // G261MatCause. Diagnostic only, and zero work when DC2_G570_BUMP_TRACE is unset.
            // FBO still equals VRAM for this target: refresh the snapshot past our own bump —
            // G351: for the FBO key actually READ; a mixed-band publication leaves neither
            // single FBO equal to VRAM, so both surface snapshots go conservative-invalid.
            if (g351ReadKey != 0u)
            {
                G178FbSnap &snap = g_g178FbSnap[g351ReadKey];
                snap.valid = true;
                if (r.covLo < snap.rowLo) snap.rowLo = r.covLo;
                if (r.covHi > snap.rowHi) snap.rowHi = r.covHi;
                snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                             static_cast<uint32_t>(r.fbW), 512u);
                if (g351ReadKey != fbp)
                {
                    G178FbSnap &phys = g_g178FbSnap[fbp];
                    phys.valid = false;
                    phys.genSum = 0;
                    phys.rowLo = 1 << 30;
                    phys.rowHi = -1;
                }
            }
            else
            {
                for (const uint32_t k : {fbp, kG351Sprite139})
                {
                    G178FbSnap &sn = g_g178FbSnap[k];
                    sn.valid = false;
                    sn.genSum = 0;
                    sn.rowLo = 1 << 30;
                    sn.rowHi = -1;
                }
            }
        }
    }
    if (!ok)
    {
        std::fprintf(stderr,
                     "[G261:materialize-fail] fbp=0x%x rows=%d..%d fbW=%d cause=%d — residency "
                     "dropped, VRAM stays authoritative\n",
                     fbp, r.dirtyLo, r.dirtyHi, r.fbW, cause);
        g_g261Mat[kG261MatFail].fetch_add(1u, std::memory_order_relaxed);
        // FBO↔VRAM equality is unknown now — force a fresh upload. Reset in place (callers may
        // hold live references into g_g178FbSnap; erase would invalidate them). G351: both
        // surface snapshots (the split routes 0x139 across two FBO keys).
        for (const uint32_t k : {fbp, kG351Sprite139})
        {
            G178FbSnap &failSnap = g_g178FbSnap[k];
            failSnap.valid = false;
            failSnap.genSum = 0;
            failSnap.rowLo = 1 << 30;
            failSnap.rowHi = -1;
            if (!(g351SplitOn() && fbp == 0x139u))
                break; // split off (or another target): original single-key behavior
        }
    }
    t_g597InMatPrePub = false; // G597: also clear on the !ok / early-return paths
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    g261UpdateWindow(ti); // window shrinks to cov rows; gen baseline re-anchored past our bump
    g310NoteTargetClean(ok);
    g_g261MatByTarget[ti & 7].fetch_add(1u, std::memory_order_relaxed);
    if (g279Profile)
    {
        g_g279MatNs[cause & 7].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279MatRows[cause & 7].fetch_add(
            static_cast<uint64_t>(std::max(rows, 0)), std::memory_order_relaxed);
    }
    const uint64_t n = g_g261Mat[cause & 7].fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 512u) == 0u)
        g261Report();
}

// ---- G276: within-drain display-color readback coalescing -------------------------------------
// Steady MAP-0 pays ~9 full-FBO color glReadPixels/frame (~30 ms/f, ~13% of the frame): each
// display batch (fbp 0x0/0x68) blocks on its own readback even when the very next batch draws into
// the SAME persistent FBO with no intervening VRAM-color consumer. The FBO accumulates across those
// batches, and the gen-snapshot already suppresses a re-upload for a same-target continuation (the
// readback does not bump color gens), so a consecutive same-target run needs exactly ONE readback,
// not K. G276 defers the readback (renderBatch skips the glReadPixels; the draw still lands in the
// FBO), records the pending union, and publishes it (one row-windowed g178_backend_read_color +
// swizzle -> VRAM) at the next different target OR any g272MaterializeAll consumer edge (GPU drain,
// CPU replay, inline primitive, drain end, l2h/l2l upload) — the same edge set G272 established.
// Scope is WITHIN A DRAIN (no persistent cross-frame residency), so the G242/G249/G272 cross-frame
// ownership trap cannot occur; VRAM is authoritative at exactly the boundaries it is today.
// (Lever g276DisplayRbOn(), the G276PendDisplay state, and its counters are declared earlier so
// g261Report can print them; only the flush body lives here where its readback helpers are visible.)

// Publish the pending coalesced display readback (union rows) into guest VRAM. Mirrors the per-batch
// color writeback exactly: row-windowed FBO readback, swizzle to VRAM, and NO color-gen bump (so the
// snapshot for a later same-target batch still equals current gens and re-upload stays suppressed —
// identical to today's per-batch display path). On any failure the snapshot is invalidated so the
// next flush re-uploads from VRAM (bounded stale-pixel, never a silent overwrite of newer bytes).
// Defined at global scope (region 4683..7342), static/internal linkage; called only from sites that
// are lexically after this point (the different-target site in g178TryFlushGpu and each
// g272MaterializeAll consumer-edge call site), so no forward declaration is needed.
// ===== G600 ORDERED TRACE (default OFF, DC2_G600_SEQ=1 + _SEQ_LO/_SEQ_HI) =====
// `[G600:direct]` says the display grab loses its FBO source because `s_g276Pend.active == 0`, and
// `[G600:disp]` says the display batch armed it (`coalesce=1`) — so the record is CLEARED between
// the two. A bucketed census cannot show that; only an ordered trace of one script frame can.
// Window it to two guest frames and it prints a few dozen lines.
static bool g600SeqOn()
{
    static const bool on = envFlagEnabled("DC2_G600_SEQ");
    return on;
}
static bool g600SeqWindow()
{
    if (!g600SeqOn())
        return false;
    static const uint32_t lo = g599EnvU32("DC2_G600_SEQ_LO", 0u);
    static const uint32_t hi = g599EnvU32("DC2_G600_SEQ_HI", 0u);
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    return sf >= lo && (hi == 0u || sf <= hi);
}
static void g600Seq(const char *what, int a, uint32_t b, int c)
{
    if (!g600SeqWindow())
        return;
    static uint64_t s_n = 0u;
    std::fprintf(stderr,
                 "[G600:seq] sf=%u i=%llu %-12s a=%d b=0x%x c=%d | pend(act=%u fbp=0x%x "
                 "rows=%d..%d)\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(++s_n), what, a, b, c,
                 s_g276Pend.active ? 1u : 0u, s_g276Pend.fbp, s_g276Pend.rowLo, s_g276Pend.rowHi);
}

// ===== G600 FIX (DEFAULT ON, rollback DC2_G600_NO_DISPLAY_PUB_GEN=1) =====
// THE COALESCED DISPLAY PUBLICATION IS A TEXTURE-PRODUCER EVENT AND WAS NOT ANNOUNCED AS ONE.
//
// Every other path that writes guest VRAM from an FBO advances the page generations G178's texture
// registry keys on, and then RE-ANCHORS its own snapshot past that bump so the upload-suppression
// property is unchanged — g261Materialize does exactly this ~200 lines above, and the per-batch
// display readback does it in rasterizer_vram_materialization.inc ("The RTT color write is a
// texture-producer event"). G276's coalesced flush skipped BOTH halves, keeping only the second
// half's *intent* ("no color-gen bump, so re-upload stays suppressed"). The consequence is not a
// suppressed upload — it is a texture registry entry over the display pages that can never be
// invalidated:
//
//   MAP-15, scriptFrames 1961..2424. The cutscene runs a full-screen post-process: a one-entry
//   `display -> 0x139` grab sprite, then `0x139 -> display`. The grab normally binds the display FBO
//   directly (g289DirectDisplayToken), which requires `s_g276Pend.active`. In this window the inline
//   CPU-primitive consumer edge (cause 4, rasterizer_tilebin_capture.inc) publishes and CLEARS the
//   pending record ~1000 flushes before the grab, so the grab falls back to the ordinary guest-VRAM
//   decode — and `[G600:hit] why=gen` on EVERY key of the window says that decode never happens: the
//   registry's first test is `genSum == genNow`, the publication never moved a generation, so the
//   GPU keeps ONE frozen copy of the display for 460 scriptFrames. It is composited back over the
//   frame, and wherever nothing overdraws it — the sky band — you see a frozen old image.
//   Measured: `mean|A-B|` over rows 0..119 between sf 1980 and 2002 is EXACTLY 0.00.
//
// The fix is the missing half, in the form the neighbours already use: bump the published rect, then
// re-anchor this target's snapshot past our own bump. Upload suppression is preserved by
// construction (the snapshot is re-sampled after the bump); the registry now sees the invalidation
// and falls through to its exact content-hash test, which re-decodes only when the bytes moved.
static bool g600DisplayPubGenOn()
{
    static const bool on = !envFlagEnabled("DC2_G600_NO_DISPLAY_PUB_GEN") &&
                           !envFlagDisabled("DC2_G600_DISPLAY_PUB_GEN");
    return on;
}
static std::atomic<uint64_t> g_g600PubGenBumps{0};

static void g276FlushPendingDisplay(int cause)
{
    g600Seq("flush.enter", cause, 0u, 0);
    if (!s_g276Pend.active)
        return;
    const bool g279Profile = g279ProfileOn();
    const auto g279T0 = g279Profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    // Preserve the historical per-batch publication order when color and Z physically alias:
    // G262 wrote Z back before color. Range-disjoint owners remain independently coalescable.
    const G144BlockRange displayRange = g144RangeForRect(
        s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
        static_cast<uint32_t>(s_g276Pend.rowLo),
        static_cast<uint32_t>(s_g276Pend.fbW),
        static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &pz = s_g278PendTab[s];
        if (!pz.active)
            continue;
        if (pz.privateMirror)
            continue;
        const G144BlockRange depthWhole = g278PendingDepthWholeRangeP(pz);
        if (!depthWhole.valid || !displayRange.valid ||
            g144RangeOverlaps(depthWhole, displayRange))
        {
            if (!g278FlushPendSlot(pz, cause))
                return;
        }
    }
    if (!g278FlushPendingDepthForRanges(
            cause, G144BlockRange{}, displayRange, false, !displayRange.valid))
        return;
    if (g277CensusOn() && cause >= 0 && cause < 9)
        g_g277FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    G276PendDisplay p = s_g276Pend;
    g600Seq("flush.clear", cause, s_g276Pend.fbp, s_g276Pend.rowHi);
    s_g276Pend.active = false;
    s_g276Pend.rowLo = 1 << 30;
    s_g276Pend.rowHi = -1;
    uint8_t *vram = g_g144LastVram;
    const uint32_t vramSize = g_g261VramSize;
    const int rows = p.rowHi - p.rowLo + 1;
    bool ok = false;
    static std::vector<uint32_t> s_g276Px; // single-threaded flush-owner path (as g261Materialize)
    if (vram != nullptr && vramSize != 0u && p.fbW > 0 && rows > 0)
    {
        const int glY = 512 - 1 - p.rowHi;
        const auto g279ReadT0 = g279Profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        const G455Ticket g455Ticket = g455BeginRead(
            kG455Display, s_g455DisplayWriteSeq, p.fbp, p.fbw, p.fbW, p.rowLo, p.rowHi);
        const bool g454Skip = g454SkipReadbackAt(kG454RbDisplayLeverId);
        if (g454Skip)
            s_g276Px.resize(static_cast<size_t>(p.fbW) * rows);
        const bool readOk = g454Skip ||
            g178_backend_read_color(p.fbp, p.fbW, 512, glY, rows, s_g276Px);
        g455EndRead(g455Ticket);
        if (g279Profile && cause >= 0 && cause < 9)
            g_g279DisplayReadNs[cause].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - g279ReadT0).count()),
                std::memory_order_relaxed);
        ok = readOk && s_g276Px.size() == static_cast<size_t>(p.fbW) * rows;
        if (ok)
        {
            // G498: the frame's LARGEST owner publish — 512 x 416 = 212,992 px, once per frame on
            // lean MAP-0, and the site `[G495:rb]` sees as the `0x0`/`0x68 @96/416` readback. The
            // loop it replaces had NO block filter, only the vramSize clip, so the whole row range
            // takes G418's promoted arm; `g498PublishOwnedRows` still classifies, because the clip
            // itself can fail near the end of VRAM and those rows must keep the original loop.
            // `glY = 511 - p.rowHi`, so the original `srcRow = (511 - y - glY)` IS `(p.rowHi - y)`,
            // i.e. G418's bottom-first convention with fbH = p.rowHi + 1.
            g498PublishOwnedRows(0, vram, vramSize, s_g276Px, p.fbBlock, p.fbw, p.fbW,
                                 p.rowHi + 1, p.rowLo, p.rowHi, 0u, 0xFFFFFFFFu);
            // G336: this display-color readback published guest VRAM rows rowLo..rowHi at
            // (fbBlock,fbw) as CT32 — capture it into the boundary's published set (no-op unless
            // DC2_G336_RUNWAY=1 and inside the boundary window).
            g336NotePublish(p.fbBlock, p.fbw, p.fbW, p.rowLo, p.rowHi);
        }
    }
    if (ok)
    {
        g_g276Flushes.fetch_add(1u, std::memory_order_relaxed);
        g_g276FlushRows.fetch_add(static_cast<uint64_t>(rows), std::memory_order_relaxed);
        // G600: guest VRAM for rows p.rowLo..p.rowHi of this display target just CHANGED. Announce
        // it on the same page generations the texture registry reads, then re-anchor this target's
        // snapshot past our own bump so the FBO<->VRAM equality the upload skip depends on is
        // exactly as it was before (g261Materialize's pattern, same order, same gen window).
        if (g600DisplayPubGenOn())
        {
            g178BumpRectImpl(p.fbBlock, p.fbw, GS_PSM_CT32, 0u,
                             static_cast<uint32_t>(p.rowLo), static_cast<uint32_t>(p.fbW),
                             static_cast<uint32_t>(rows),
                             2u | (static_cast<uint32_t>(cause & 0xff) << 8u));
            G178FbSnap &snap = g_g178FbSnap[p.fbp];
            if (snap.valid)
                snap.genSum = g178GenSumRect(p.fbBlock, p.fbw, GS_PSM_CT32, 0u, 0u,
                                             static_cast<uint32_t>(p.fbW), 512u);
            g_g600PubGenBumps.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    else
    {
        // FBO<->VRAM relation now unknown for this target — force a fresh upload next flush.
        g_g276FlushFail.fetch_add(1u, std::memory_order_relaxed);
        G178FbSnap &snap = g_g178FbSnap[p.fbp];
        snap.valid = false;
        snap.genSum = 0;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    if (g279Profile && cause >= 0 && cause < 9)
    {
        g_g279DisplayNs[cause].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - g279T0).count()),
            std::memory_order_relaxed);
        g_g279DisplayN[cause].fetch_add(1u, std::memory_order_relaxed);
    }
}

// ---- G277 census helpers (counters only, DC2_G277_CENSUS=1; state by s_g276Pend) ---------------
// Block range of the CURRENT pending coalesced display readback (invalid when none).
static G144BlockRange g277PendingDisplayRange()
{
    if (!s_g276Pend.active || s_g276Pend.fbW <= 0 || s_g276Pend.rowHi < s_g276Pend.rowLo)
        return {};
    return g144RangeForRect(s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
                            static_cast<uint32_t>(s_g276Pend.rowLo),
                            static_cast<uint32_t>(s_g276Pend.fbW),
                            static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
}

// G284: publish the pending coalesced display readback at a RANGED transfer edge (cause 7 l2h dst,
// cause 8 l2l src+dst) only if that transfer could observe or overwrite the pending display rows.
// Lever-off, unknown range, degenerate pending, or overlap => the original unconditional G276 flush
// (bit-identical to pre-G284). A provably-disjoint transfer keeps the display readback coalesced for
// the next REAL consumer edge — exactly how G278 already gates the depth readback at this same edge.
static void g276FlushPendingDisplayForRanges(int cause, const G144BlockRange &src,
                                             const G144BlockRange &dst, bool srcUnknown,
                                             bool dstUnknown)
{
    if (!s_g276Pend.active)
        return;
    if (g284UploadCoalesceOn() && !srcUnknown && !dstUnknown)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const bool overlaps = (src.valid && g144RangeOverlaps(pend, src)) ||
                              (dst.valid && g144RangeOverlaps(pend, dst));
        if (pend.valid && !overlaps)
        {
            if (g284StatOn())
                g_g284UploadSkipped.fetch_add(1u, std::memory_order_relaxed);
            return; // disjoint: keep the display readback coalesced
        }
    }
    if (g284StatOn())
        g_g284UploadFlushed.fetch_add(1u, std::memory_order_relaxed);
    g276FlushPendingDisplay(cause);
}

// G288: consumer-complete bridge for a G284-deferred display owner into the next native GPU
// batch. g178TryFlushGpu calls this before framebuffer upload, texture/CLUT decode, or depth setup.
// Same-target color writes deliberately continue in the persistent FBO (the proven G276 path);
// every other physical read/write range is checked against the unpublished display rows.
static void g288PublishDisplayForGpuBatch()
{
    if (!g288GpuConsumerOn() || !s_g276Pend.active || g_g144List.empty())
        return;

    g_g288GpuAudits.fetch_add(1u, std::memory_order_relaxed);
    const G144BlockRange pending = g277PendingDisplayRange();
    bool overlap = false;
    bool unknown = !pending.valid;
    uint32_t reason = unknown ? 0x10u : 0u; // 1=color, 2=depth, 4=texture, 8=CLUT, 16=unknown
    size_t hitEntry = 0u;

    for (size_t i = 0; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool sameDisplay =
            e.ctx.frame.fbp == s_g276Pend.fbp && e.ctx.frame.fbw == s_g276Pend.fbw;

        if (!sameDisplay)
        {
            const G144BlockRange color = g144TargetRange(e);
            const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                                  e.ctx.scissor.y1 >= e.ctx.scissor.y0;
            if (nonEmpty && !color.valid)
            {
                unknown = true;
                reason |= 0x11u;
            }
            else if (color.valid && g144RangeOverlaps(pending, color))
            {
                overlap = true;
                reason |= 0x1u;
            }
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(pending, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(pending, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(pending, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }

    if (!overlap && !unknown)
    {
        g_g288GpuDisjoint.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if (unknown)
        g_g288GpuUnknown.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t n =
        g_g288GpuPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g288StatOn() && (n <= 64u || (n % 256u) == 0u))
    {
        const G144Entry &e = g_g144List[std::min(hitEntry, g_g144List.size() - 1u)];
        std::fprintf(stderr,
                     "[G288:gpu-consumer] n=%llu reason=0x%x entry=%zu/%zu "
                     "pend(fbp=0x%x fbw=%u rows=%d..%d blocks=%x..%x) "
                     "draw(fbp=0x%x fbw=%u tme=%u tbp=0x%x psm=0x%x cbp=0x%x)\n",
                     static_cast<unsigned long long>(n), reason, hitEntry, g_g144List.size(),
                     s_g276Pend.fbp, s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                     pending.first, pending.last, e.ctx.frame.fbp, e.ctx.frame.fbw,
                     e.prim.tme ? 1u : 0u, e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.cbp);
    }
    g276FlushPendingDisplay(0);
}

// Private .cpp-to-.cpp bridge; keeping G289 out of the shared LLE header avoids widening the
// backend surface for one tightly admitted owner.
extern bool g289_backend_copy_display_to_work(uint32_t srcFbp, uint32_t dstFbp);
extern bool g289_backend_build_work_alias_view();
static bool g289Resolve14aBeforeDirect();

// ===== G600 census (default OFF, DC2_G600_DIRECT=1): WHICH CONJUNCT OF THE DISPLAY GRAB FAILS =====
// The MAP-15 defect window is exactly the interval over which this token is 0. When it is 0 the
// full-screen `display -> 0x139` grab falls back to decoding guest VRAM at the display base, which
// is stale because the display target is GPU-resident — so the post-process composites a FROZEN
// old frame back over the sky. G590's law applies: count EVERY conjunct, or the lever names nothing.
static bool g600DirectOn()
{
    static const bool on = envFlagEnabled("DC2_G600_DIRECT");
    return on;
}

// Fires only on flushes that plausibly ARE the grab (some entry draws a CT32 textured sprite into
// 0x139), at most once per script-frame bucket per reason, so the log stays bounded.
static void g600ReportDirect(const char *why, uint32_t shapeBits)
{
    bool candidate = false;
    for (const G144Entry &e : g_g144List)
        if (e.ctx.frame.fbp == 0x139u && e.prim.type == GS_PRIM_SPRITE && e.prim.tme &&
            e.ctx.tex0.psm == GS_PSM_CT32 && e.ctx.tex0.tbw == 8u)
        {
            candidate = true;
            break;
        }
    if (!candidate)
        return;
    static const uint32_t every = g599EnvU32("DC2_G600_DIRECT_EVERY", 22u);
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    static uint32_t s_bucket = 0xFFFFFFFFu;
    static uint32_t s_seenMask = 0u;
    const uint32_t bucket = sf / (every ? every : 1u);
    if (bucket != s_bucket)
    {
        s_bucket = bucket;
        s_seenMask = 0u;
    }
    const uint32_t bit = 1u << (why[0] & 31u);
    if ((s_seenMask & bit) != 0u)
        return;
    s_seenMask |= bit;
    const G144Entry &e = g_g144List[0]; // the candidate scan above proves the list is non-empty
    std::fprintf(stderr,
                 "[G600:direct] sf=%u why=%s n=%zu localCopy=%u pend(act=%u fbp=0x%x fbw=%u "
                 "fbW=%d rows=%d..%d) e0(fbp=0x%x fbw=%u psm=%u prim=%u tme=%u fst=%u tbp=0x%x "
                 "tbw=%u tpsm=%u tw=%u th=%u sciss=%u,%u..%u,%u uv=%u,%u..%u,%u) shape=0x%x "
                 "owner=%u\n",
                 sf, why, g_g144List.size(), g289LocalCopyOn() ? 1u : 0u,
                 s_g276Pend.active ? 1u : 0u, s_g276Pend.fbp, s_g276Pend.fbw, s_g276Pend.fbW,
                 s_g276Pend.rowLo, s_g276Pend.rowHi,
                 e.ctx.frame.fbp, e.ctx.frame.fbw, static_cast<unsigned>(e.ctx.frame.psm),
                 static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u, e.prim.fst ? 1u : 0u,
                 e.ctx.tex0.tbp0, e.ctx.tex0.tbw, static_cast<unsigned>(e.ctx.tex0.psm),
                 static_cast<unsigned>(e.ctx.tex0.tw), static_cast<unsigned>(e.ctx.tex0.th),
                 e.ctx.scissor.x0, e.ctx.scissor.y0, e.ctx.scissor.x1, e.ctx.scissor.y1,
                 e.v[0].u, e.v[0].v, e.v[1].u, e.v[1].v, shapeBits,
                 g345AnyOwnerActive() ? 1u : 0u);
}

// The exact display-grab sprite previously proven by G288's direct-source prototype. The one-row
// bilinear halo is safe because CLAMP repeats GS row 415; no undefined FBO row is sampled.
static uint32_t g289DirectDisplayToken()
{
    const bool g600 = g600DirectOn();
    if (!g289LocalCopyOn() || !s_g276Pend.active ||
        s_g276Pend.fbW != 512 || s_g276Pend.fbw != 8u ||
        s_g276Pend.rowLo != 0 || s_g276Pend.rowHi != 415 ||
        (s_g276Pend.fbp != 0u && s_g276Pend.fbp != 0x68u) ||
        g_g144List.size() != 1u)
    {
        if (g600)
            g600ReportDirect("pend", 0u);
        if (!g_g144List.empty() && g_g144List[0].ctx.frame.fbp == 0x139u)
            g600Seq("grab.NOPEND", static_cast<int>(g_g144List.size()),
                    g_g144List[0].ctx.tex0.tbp0, 0);
        return 0u;
    }

    const G144Entry &e = g_g144List[0];
    const uint32_t sourceBlock = framePageBaseToBlock(s_g276Pend.fbp);
    const bool exact =
        e.ctx.frame.fbp == 0x139u && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 447u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && e.prim.fst &&
        e.ctx.tex0.tbp0 == sourceBlock && e.ctx.tex0.tbw == 8u &&
        e.ctx.tex0.psm == GS_PSM_CT32 &&
        e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u &&
        tex1UsesLinearFilter(e.ctx.tex1) &&
        e.v[0].u == 0u && e.v[0].v == 0u &&
        e.v[1].u == 512u * 16u && e.v[1].v == 416u * 16u;
    if (!exact)
    {
        if (g600)
        {
            // One bit per conjunct of `exact`, in source order, so the census names the failure
            // instead of reporting that "the shape did not match".
            const uint32_t bits =
                (e.ctx.frame.fbp == 0x139u ? 0u : 1u << 0) |
                (e.ctx.frame.fbw == 8u ? 0u : 1u << 1) |
                (e.ctx.frame.psm == GS_PSM_CT32 ? 0u : 1u << 2) |
                ((e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
                  e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 447u) ? 0u : 1u << 3) |
                (e.prim.type == GS_PRIM_SPRITE ? 0u : 1u << 4) |
                (e.prim.tme ? 0u : 1u << 5) |
                (e.prim.fst ? 0u : 1u << 6) |
                (e.ctx.tex0.tbp0 == sourceBlock ? 0u : 1u << 7) |
                (e.ctx.tex0.tbw == 8u ? 0u : 1u << 8) |
                (e.ctx.tex0.psm == GS_PSM_CT32 ? 0u : 1u << 9) |
                (e.ctx.tex0.tw == 9u ? 0u : 1u << 10) |
                (e.ctx.tex0.th == 9u ? 0u : 1u << 11) |
                (tex1UsesLinearFilter(e.ctx.tex1) ? 0u : 1u << 12) |
                ((e.v[0].u == 0u && e.v[0].v == 0u) ? 0u : 1u << 13) |
                ((e.v[1].u == 512u * 16u && e.v[1].v == 416u * 16u) ? 0u : 1u << 14);
            g600ReportDirect("shape", bits);
        }
        return 0u;
    }
    if (g345AnyOwnerActive() && !g289Resolve14aBeforeDirect())
    {
        if (g600)
            g600ReportDirect("resolve14a", 0u);
        return 0u;
    }
    if (g345AnyOwnerActive())
    {
        if (g600)
            g600ReportDirect("g345owner", 0u);
        return 0u; // G346: another slot survived the 0x14a resolve — fail closed to normal path
    }
    if (g600)
        g600ReportDirect("bind", 0u);
    g600Seq("grab.BIND", 0, s_g276Pend.fbp, 0);
    return s_g276Pend.fbp == 0u ? kG289ZeroFbpToken : s_g276Pend.fbp;
}

// G346: union across every active slot. Overlap consumers become fail-closed conservative for
// multi-slot states; closure-off (single active slot at most) this is the original range.
static G144BlockRange g289OwnerRange()
{
    G144BlockRange out;
    out.valid = false;
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        const G289CopyOwner &o = s_g289Owners[i];
        if (!o.active || o.firstOwnedBlock > o.lastOwnedBlock)
            continue;
        if (!out.valid)
        {
            out.first = o.firstOwnedBlock;
            out.last = o.lastOwnedBlock;
            out.valid = true;
        }
        else
        {
            out.first = std::min(out.first, o.firstOwnedBlock);
            out.last = std::max(out.last, o.lastOwnedBlock);
        }
    }
    return out;
}

// G572 (default-ON, rollback DC2_G572_NO_EXACT_OWNER_GEN=1): G289 publishes only the physical blocks still owned by its FBO,
// but historically bumped generations for the whole rectangular surface.  That makes unrelated
// resident targets look guest-written even though g498PublishOwnedRows filtered every such byte.
// A GS generation is an invalidation epoch, not a write count, so one increment for every
// physical 8 KB page intersecting the actually written block interval is exact.
static bool g572ExactOwnerGenOn()
{
    static const bool on = !envFlagEnabled("DC2_G572_NO_EXACT_OWNER_GEN") &&
                           !envFlagDisabled("DC2_G572_EXACT_OWNER_GEN");
    return on;
}

static void g572BumpOwnedBlockRange(uint32_t firstBlock, uint32_t lastBlock)
{
    if (firstBlock > lastBlock)
        return;
    const uint32_t firstPage = firstBlock >> 5u;
    const uint32_t lastPage = lastBlock >> 5u;
    if (g_g633GuestWriteHook != nullptr)
        g_g633GuestWriteHook(firstPage, lastPage - firstPage + 1u,
                             0u, 0u, 0u, lastPage - firstPage, true, 17u);
    const bool traceOn = g570BumpTraceOn();
    G570BumpTrace trace{};
    if (traceOn)
    {
        trace.seq = g_g570BumpSeq.fetch_add(1u, std::memory_order_relaxed) + 1u;
        trace.source = 6u; // exact G289 owner publication
        trace.bpBlocks = firstBlock;
        trace.bw64 = 0u;
        trace.psm = 0xffu; // diagnostic sentinel: physical block interval, not a PSM rect
        trace.w = lastBlock - firstBlock + 1u;
        trace.h = 1u;
    }
    for (uint32_t page = firstPage;; ++page)
    {
        const uint32_t physical = page & (kG178PageCount - 1u);
        g_g178PageGen[physical].fetch_add(1u, std::memory_order_relaxed);
        if (traceOn)
            g_g570LastBump[physical] = trace;
        if (page == lastPage)
            break;
    }
    if (g289StatOn())
    {
        static uint64_t s_n = 0u;
        ++s_n;
        if (s_n <= 8u || (s_n % 128u) == 0u)
            std::fprintf(stderr,
                         "[G572:owner-gen] n=%llu blocks=%x..%x pages=%x..%x\n",
                         static_cast<unsigned long long>(s_n), firstBlock, lastBlock,
                         firstPage, lastPage);
    }
}

static bool g345MaterializeSlot(G289CopyOwner &slot, int cause)
{
    if (!slot.active)
        return true;
    G289CopyOwner owner = slot;
    slot.active = false;
    if (&slot == &s_g289Owners[0])
        s_g289PendingUpload.active = false;

    static std::vector<uint32_t> s_px;
    const bool ok =
        owner.vram != nullptr && owner.vramSize != 0u &&
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 416, s_px) &&
        s_px.size() == 512u * 416u;
    if (ok)
    {
        const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
        // G498: 212,992 px/frame. The per-pixel loop this replaces is reproduced verbatim inside
        // g498PublishRowsLegacy and runs for every row the owner's block filter does not cover.
        g498PublishOwnedRows(0, owner.vram, owner.vramSize, s_px, dstBlock, 8u, 512, 416,
                             0, 415, owner.firstOwnedBlock, owner.lastOwnedBlock);
        if (g572ExactOwnerGenOn())
            g572BumpOwnedBlockRange(owner.firstOwnedBlock, owner.lastOwnedBlock);
        else
            g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 0u, 512u, 416u,
                             18u); // G582 source 18: full 512x416 owner materialize
        g_g289Materializes.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        g_g289MatFail.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G289:materialize-fail] version=%llu cause=%d vram=%p size=%u\n",
                     static_cast<unsigned long long>(owner.version), cause,
                     static_cast<void *>(owner.vram), owner.vramSize);
    }
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    if (ok && g345ClosureOn())
    {
        // G346 write-side retirement: the readback above published the FULL 416-row FBO content
        // and bumped those pages' generations, so FBO == VRAM for rows 0..415 of dstFbp right
        // now. Revalidating (the proven G272-materialize pattern) lets the next wave skip the
        // stale re-upload that G343 measured as the conserving edge. Non-closure keeps the
        // original conservative invalidation.
        snap.valid = true;
        snap.genSum = g178GenSumRect(framePageBaseToBlock(owner.dstFbp), 8u, GS_PSM_CT32,
                                     0u, 0u, 512u, 512u);
        snap.rowLo = std::min(snap.rowLo, 0);
        snap.rowHi = std::max(snap.rowHi, 415);
        g_g345SnapRevalidates.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    if (g345ClosureOn())
        g345NoteEscMat(cause, owner.dstFbp, 416u);
    if (g289StatOn())
    {
        const uint64_t n =
            g_g289Materializes.load(std::memory_order_relaxed) +
            g_g289MatFail.load(std::memory_order_relaxed);
        if (n <= 8u || (n % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:materialize] n=%llu version=%llu dst=%03x "
                         "owner=%x..%x cause=%d ok=%u\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(owner.version), owner.dstFbp,
                         owner.firstOwnedBlock, owner.lastOwnedBlock, cause, ok ? 1u : 0u);
    }
    return ok;
}

// Fail-closed edge: resolve EVERY active owner slot (closure-off: only slot 0 can be active).
static bool g289MaterializeOwner(int cause)
{
    bool ok = true;
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
        if (s_g289Owners[i].active)
            ok = g345MaterializeSlot(s_g289Owners[i], cause) && ok;
    return ok;
}

// The measured 512x384 PSMT4 uploads and following CT32 page uploads replace complete physical
// pages at the front of the 0x14a owner. When one intersects that prefix without a gap, guest VRAM
// owns the replaced pages after IMAGE completes and the FBO keeps owning the untouched suffix.
static bool g289PrepareUploadOverwrite(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                       uint32_t dsax, uint32_t dsay,
                                       uint32_t rrw, uint32_t rrh,
                                       const G144BlockRange &dst)
{
    const bool exactT4 =
        dbw == 8u && dpsm == GS_PSM_T4 &&
        dsax == 0u && dsay == 0u && rrw == 512u && rrh == 384u;
    const bool exactCt32 =
        dbw == 2u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 128u && rrh == 128u;
    const bool exactCt32Tail =
        dbw == 1u && dpsm == GS_PSM_CT32 &&
        dsax == 0u && dsay == 0u && rrw == 64u && rrh == 64u;
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    if (o14a == nullptr ||
        (!exactT4 && !exactCt32 && !exactCt32Tail) ||
        !dst.valid || dst.first > o14a->firstOwnedBlock ||
        dst.last < o14a->firstOwnedBlock ||
        o14a->lastOwnedBlock != 0x363fu)
        return false;
    s_g289PendingUpload.active = true;
    s_g289PendingUpload.ownerVersion = o14a->version;
    s_g289PendingUpload.dbp = dbp;
    s_g289PendingUpload.dbw = dbw;
    s_g289PendingUpload.dpsm = dpsm;
    s_g289PendingUpload.dsax = dsax;
    s_g289PendingUpload.dsay = dsay;
    s_g289PendingUpload.rrw = rrw;
    s_g289PendingUpload.rrh = rrh;
    s_g289PendingUpload.replacedThroughBlock = dst.last;
    return true;
}

// Called after GS::processImageData has written the complete exact image.
void g289NoteUploadComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                            uint32_t dsax, uint32_t dsay,
                            uint32_t rrw, uint32_t rrh)
{
    if (!s_g289PendingUpload.active)
        return;
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    const bool exact =
        o14a != nullptr &&
        o14a->version == s_g289PendingUpload.ownerVersion &&
        dbp == s_g289PendingUpload.dbp &&
        dbw == s_g289PendingUpload.dbw &&
        dpsm == s_g289PendingUpload.dpsm &&
        dsax == s_g289PendingUpload.dsax &&
        dsay == s_g289PendingUpload.dsay &&
        rrw == s_g289PendingUpload.rrw &&
        rrh == s_g289PendingUpload.rrh;
    if (!exact)
    {
        s_g289PendingUpload.active = false;
        return;
    }
    o14a->firstOwnedBlock =
        std::max(o14a->firstOwnedBlock,
                 s_g289PendingUpload.replacedThroughBlock + 1u);
    s_g289PendingUpload.active = false;
    const bool fullyReplaced =
        o14a->firstOwnedBlock > o14a->lastOwnedBlock;
    if (fullyReplaced)
    {
        o14a->active = false;
        G178FbSnap &snap = g_g178FbSnap[o14a->dstFbp];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
    const uint64_t n =
        g_g289UploadOverwrites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:upload-overwrite] n=%llu version=%llu "
                     "upload=%x..%x owner=%x..%x full=%u\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(o14a->version),
                     dbp, s_g289PendingUpload.replacedThroughBlock,
                     o14a->firstOwnedBlock, o14a->lastOwnedBlock,
                     fullyReplaced ? 1u : 0u);
}

// G314 cross-TU bridge from GS::processImageData. Arm first-consumer tracking only after every
// IMAGE chunk has landed; otherwise continuation chunks would be misclassified as consumers.
// This sits beside the proven G289 completion bridge so it has the same external linkage.
void g314NoteImageComplete(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                           uint32_t dsax, uint32_t dsay,
                           uint32_t rrw, uint32_t rrh)
{
    if (!g314CensusOn() || !g_g314Pending.waitingForComplete)
        return;
    const bool exact = g_g314Pending.dbp == dbp && g_g314Pending.dbw == dbw &&
                       g_g314Pending.dpsm == dpsm && g_g314Pending.dsax == dsax &&
                       g_g314Pending.dsay == dsay && g_g314Pending.rrw == rrw &&
                       g_g314Pending.rrh == rrh;
    g_g314Pending.waitingForComplete = false;
    if (!exact || g_g314Pending.shape == nullptr || !g_g314Pending.range.valid)
    {
        g_g314Pending.shape = nullptr;
        return;
    }
    g_g314Pending.armed = true;
}

// The following exact fbp=0x139 display sprite overwrites physical pages 0x2720..0x341f.
// For the partially overwritten 0x14a owner, only page 0x3420 onward can survive. Publishing
// GS rows 320..415 covers that complete suffix (plus harmless bounded over-work), after which
// the normal direct sprite may execute with no unresolved alias owner.
static bool g289Resolve14aBeforeDirect()
{
    G289CopyOwner *o14a = g345OwnerFor(0x14au);
    if (o14a == nullptr ||
        o14a->firstOwnedBlock < 0x29a0u ||
        o14a->firstOwnedBlock > o14a->lastOwnedBlock ||
        o14a->lastOwnedBlock != 0x363fu ||
        o14a->vram == nullptr || o14a->vramSize == 0u)
        return false;

    G289CopyOwner owner = *o14a;
    static std::vector<uint32_t> s_px;
    const bool readOk =
        g178_backend_read_color(owner.dstFbp, 512, 512, 96, 96, s_px) &&
        s_px.size() == 512u * 96u;
    if (!readOk)
        return g289MaterializeOwner(12);

    const uint32_t dstBlock = framePageBaseToBlock(owner.dstFbp);
    // G498: 49,152 px/frame. `s_px` holds only the 96 read rows, and the original loop indexed it
    // as (415 - y) with y in [320,415] — i.e. exactly the fbH=416 bottom-first convention
    // g418UnpackColorRows already uses, so the same (fbH, rowLo, rowHi) triple reproduces it.
    g498PublishOwnedRows(1, owner.vram, owner.vramSize, s_px, dstBlock, 8u, 512, 416,
                         320, 415, owner.firstOwnedBlock, owner.lastOwnedBlock);
    if (g572ExactOwnerGenOn())
    {
        const G144BlockRange rows =
            g144RangeForRect(dstBlock, GS_PSM_CT32, 8u, 0u, 320u, 512u, 96u);
        if (rows.valid)
            g572BumpOwnedBlockRange(std::max(owner.firstOwnedBlock, rows.first),
                                    std::min(owner.lastOwnedBlock, rows.last));
        else
            g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 320u, 512u, 96u,
                             19u); // G582 source 19: lower-strip owner materialize (A)
    }
    else
    {
        g178BumpRectImpl(dstBlock, 8u, GS_PSM_CT32, 0u, 320u, 512u, 96u,
                         20u); // G582 source 20: lower-strip owner materialize (B)
    }
    o14a->active = false;
    s_g289PendingUpload.active = false;
    if (g345StatOn())
        g345NoteFamReadbackFbp(owner.dstFbp, 96u);
    G178FbSnap &snap = g_g178FbSnap[owner.dstFbp];
    snap.valid = false;
    snap.genSum = 0u;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    const uint64_t n =
        g_g289SuffixPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (n <= 16u || (n % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:suffix-publish] n=%llu version=%llu dst=14a rows=320..415\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(owner.version));
    return true;
}

static void g289MaterializeForRanges(const G144BlockRange &src,
                                     const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (srcUnknown || dstUnknown)
    {
        if (g345AnyOwnerActive())
            (void)g289MaterializeOwner(cause);
        return;
    }
    // G346 partial escape (contract I3): only the slots the ranges actually touch pay; the rest
    // stay resident. Closure-off at most slot 0 is active, so behavior is unchanged.
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active)
            continue;
        G144BlockRange range;
        range.first = o.firstOwnedBlock;
        range.last = o.lastOwnedBlock;
        range.valid = o.firstOwnedBlock <= o.lastOwnedBlock;
        if (!range.valid ||
            (src.valid && g144RangeOverlaps(range, src)) ||
            (dst.valid && g144RangeOverlaps(range, dst)))
            (void)g345MaterializeSlot(o, cause);
    }
}

static void g310NotePresentationRead(uint32_t fbp, uint32_t fbw, uint32_t psm,
                                     uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     bool useLocalMemoryLayout, bool frameBaseIsPages)
{
    if (!g310StatOn() || w == 0u || h == 0u)
        return;
    g_g310PresentReads.fetch_add(1u, std::memory_order_relaxed);
    if (!useLocalMemoryLayout)
    {
        g_g310PresentUnknown.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const uint32_t baseBlock = frameBaseIsPages ? framePageBaseToBlock(fbp) : fbp;
    const G144BlockRange read =
        g144RangeForRect(baseBlock, static_cast<uint8_t>(psm),
                         fbw != 0u ? fbw : 1u, x, y, w, h);
    if (!read.valid)
    {
        g_g310PresentUnknown.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    static constexpr uint32_t kStripFirst = 0x2720u;
    static constexpr uint32_t kStripLast = 0x371fu;
    const uint32_t lo = std::max(read.first, kStripFirst);
    const uint32_t hi = std::min(read.last, kStripLast);
    if (lo <= hi)
    {
        g_g310PresentOverlap.fetch_add(1u, std::memory_order_relaxed);
        g_g310PresentPages.fetch_add(
            static_cast<uint64_t>((hi >> 5u) - (lo >> 5u) + 1u),
            std::memory_order_relaxed);
    }
}

// Private .cpp-to-.cpp presentation bridge. Normal presentation may read the display frames,
// preferred source, or a non-black context fallback. Audit the exact source rectangle at the
// point of read; an overlap or layout we cannot prove materializes before the host copy.
void g289_prepare_presentation_read(uint8_t *vram,
                                    uint32_t fbp, uint32_t fbw, uint32_t psm,
                                    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    bool useLocalMemoryLayout, bool frameBaseIsPages)
{
    g310NotePresentationRead(
        fbp, fbw, psm, x, y, w, h, useLocalMemoryLayout, frameBaseIsPages);
    if (!g345AnyOwnerActive() || w == 0u || h == 0u)
        return;
    bool vramMismatch = !useLocalMemoryLayout;
    for (uint32_t i = 0u; i < kG345OwnerSlots && !vramMismatch; ++i)
        if (s_g289Owners[i].active && vram != s_g289Owners[i].vram)
            vramMismatch = true;
    if (vramMismatch)
    {
        (void)g289MaterializeOwner(13);
        return;
    }
    const uint32_t baseBlock = frameBaseIsPages ? framePageBaseToBlock(fbp) : fbp;
    const G144BlockRange read =
        g144RangeForRect(baseBlock, static_cast<uint8_t>(psm),
                         fbw != 0u ? fbw : 1u, x, y, w, h);
    g289MaterializeForRanges(read, G144BlockRange{}, !read.valid, false, 13);
}

struct G289InlineAudit
{
    bool carry = false;
    bool empty = false;
    bool unknown = false;
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    G144BlockRange color{}, depth{}, texture{}, clut{};
};

// Range-exact inline consumer edge. A clipped-out primitive touches nothing; otherwise the
// scissor-clipped target/depth bbox and whole texture/CLUT footprints must all provably miss the
// surviving owner. Unknown ranges and every overlap retain fail-closed cause-4 materialization.
static G289InlineAudit g289AuditInline(const GSPrimReg &prim, const GSVertex *verts,
                                       const GSContext &ctx, const GSTexClutReg &texclut)
{
    G289InlineAudit out;
    int count = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: count = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: count = 2; break;
    default: count = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minX = verts[0].x, maxX = verts[0].x;
    float minY = verts[0].y, maxY = verts[0].y;
    for (int i = 1; i < count; ++i)
    {
        minX = std::min(minX, verts[i].x);
        maxX = std::max(maxX, verts[i].x);
        minY = std::min(minY, verts[i].y);
        maxY = std::max(maxY, verts[i].y);
    }
    out.x0 = std::max(static_cast<int>(std::floor(minX)) - ofx,
                      static_cast<int>(ctx.scissor.x0));
    out.x1 = std::min(static_cast<int>(std::ceil(maxX)) - ofx,
                      static_cast<int>(ctx.scissor.x1));
    out.y0 = std::max(static_cast<int>(std::floor(minY)) - ofy,
                      static_cast<int>(ctx.scissor.y0));
    out.y1 = std::min(static_cast<int>(std::ceil(maxY)) - ofy,
                      static_cast<int>(ctx.scissor.y1));
    if (out.x1 < out.x0 || out.y1 < out.y0)
    {
        out.empty = true;
        out.carry = true;
        return out;
    }

    const uint32_t x = static_cast<uint32_t>(out.x0);
    const uint32_t y = static_cast<uint32_t>(out.y0);
    const uint32_t w = static_cast<uint32_t>(out.x1 - out.x0 + 1);
    const uint32_t h = static_cast<uint32_t>(out.y1 - out.y0 + 1);
    out.color = g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                                 ctx.frame.fbw, x, y, w, h);
    const bool depthActive = ((ctx.test >> 16u) & 1u) != 0u ||
                             ((ctx.zbuf >> 32u) & 1u) == 0u;
    if (depthActive)
        out.depth = g144RangeForRect(
            static_cast<uint32_t>(ctx.zbuf & 0x1FFu) << 5u,
            static_cast<uint8_t>((ctx.zbuf >> 24u) & 0xFu), ctx.frame.fbw,
            x, y, w, h);
    if (prim.tme)
    {
        G144Entry cand{};
        cand.ctx = ctx;
        cand.prim = prim;
        cand.texclut = texclut;
        out.texture = g144TextureRange(cand);
        if (g144IsPalettedPsm(ctx.tex0.psm))
            out.clut = g144ClutRange(cand);
    }
    out.unknown = !out.color.valid ||
                  (depthActive && !out.depth.valid) ||
                  (prim.tme && !out.texture.valid) ||
                  (prim.tme && g144IsPalettedPsm(ctx.tex0.psm) && !out.clut.valid);
    const G144BlockRange owner = g289OwnerRange();
    out.carry = !out.unknown && owner.valid &&
                !g144RangeOverlaps(owner, out.color) &&
                !g144RangeOverlaps(owner, out.depth) &&
                !g144RangeOverlaps(owner, out.texture) &&
                !g144RangeOverlaps(owner, out.clut);
    return out;
}

// G346: the exact display-grab shape (the l2l family this slice closes), src display fbp derived
// from sbp — never from the pend (the closure arm can admit with the pend already flushed).
static bool g345GrabShape(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                          uint32_t ssax, uint32_t ssay,
                          uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                          uint32_t dsax, uint32_t dsay,
                          uint32_t rrw, uint32_t rrh, uint32_t &srcFbpOut)
{
    srcFbpOut = 0xffffffffu;
    const bool shape =
        sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
        (dbp == framePageBaseToBlock(0x13bu) ||
         dbp == framePageBaseToBlock(0x14au)) &&
        dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
        rrw == 512u && rrh == 416u;
    if (!shape)
        return false;
    if (sbp == framePageBaseToBlock(0u))
        srcFbpOut = 0u;
    else if (sbp == framePageBaseToBlock(0x68u))
        srcFbpOut = 0x68u;
    else
        return false;
    return true;
}

// G346 closure source authority: the src display FBO provably equals guest VRAM for rows 0..415
// (valid snapshot covering the rows whose stored gen sum still matches the CURRENT sum, computed
// with the same window the wave upload gate uses — the G277 416-row aligned window for the 0x68
// tuple). Then the GPU FBO->FBO copy reads exactly the bytes the CPU copy would read from VRAM;
// a pending deferred readback only means the FBO is NEWER, which is still the copy's source of
// truth. Any doubt fails closed to the CPU copy.
static bool g345SnapshotSourceAuthoritative(uint32_t srcFbp, uint32_t sbp)
{
    const auto it = g_g178FbSnap.find(srcFbp);
    if (it == g_g178FbSnap.end())
        return false;
    const G178FbSnap &snap = it->second;
    if (!snap.valid || snap.rowLo > 0 || snap.rowHi < 415)
        return false;
    const uint64_t genNow = (srcFbp == 0x68u && snap.rowHi <= 415)
        ? g277GenSumAligned(sbp, 8u, 416u)
        : g178GenSumRect(sbp, 8u, GS_PSM_CT32, 0u, 0u, 512u, 512u);
    return genNow == snap.genSum;
}

static bool g289CanDeferLocalCopy(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                  uint32_t ssax, uint32_t ssay,
                                  uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay,
                                  uint32_t rrw, uint32_t rrh)
{
    bool can = g289LocalCopyOn() && !s_g289Owner.active && s_g276Pend.active &&
           s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
           s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
           framePageBaseToBlock(s_g276Pend.fbp) == sbp &&
           sbw == 8u && spsm == GS_PSM_CT32 && ssax == 0u && ssay == 0u &&
           (dbp == framePageBaseToBlock(0x13bu) ||
            dbp == framePageBaseToBlock(0x14au)) &&
           dbw == 8u && dpsm == GS_PSM_CT32 && dsax == 0u && dsay == 0u &&
           rrw == 512u && rrh == 416u;
    if (!can && g345ClosureOn() && g289LocalCopyOn())
    {
        // G346 widened admission: (a) a secondary slot or a same-dst owner reuse covers the
        // single-slot reject; (b) an exact pend for this src OR snapshot source authority covers
        // the pend-inactive reject. Everything else still falls to the CPU copy (counted escape).
        uint32_t srcFbp = 0xffffffffu;
        if (g345GrabShape(sbp, sbw, spsm, ssax, ssay,
                          dbp, dbw, dpsm, dsax, dsay, rrw, rrh, srcFbp))
        {
            const bool pendSrc = s_g276Pend.active &&
                s_g276Pend.fbW == 512 && s_g276Pend.fbw == 8u &&
                s_g276Pend.rowLo == 0 && s_g276Pend.rowHi == 415 &&
                s_g276Pend.fbp == srcFbp;
            const bool snapSrc = !pendSrc && g345SnapshotSourceAuthoritative(srcFbp, sbp);
            const uint32_t dstFbp = dbp >> 5u;
            const bool slotOk =
                g345OwnerFor(dstFbp) != nullptr || g345FreeSlot() != nullptr;
            can = (pendSrc || snapSrc) && slotOk;
        }
    }
    if (!can && g289LocalCopyOn() && s_g276Pend.active && g289StatOn())
    {
        static std::atomic<uint32_t> s_logs{0};
        const uint32_t n = s_logs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 16u)
            std::fprintf(
                stderr,
                "[G289:defer-reject] n=%u owner=%u pend=%x/%u/%d..%d/%d "
                "copy(src=%x/%u/%x/%u,%u dst=%x/%u/%x/%u,%u size=%ux%u)\n",
                n + 1u, s_g289Owner.active ? 1u : 0u, s_g276Pend.fbp,
                s_g276Pend.fbw, s_g276Pend.rowLo, s_g276Pend.rowHi,
                s_g276Pend.fbW, sbp, static_cast<unsigned>(sbw), spsm, ssax, ssay,
                dbp, static_cast<unsigned>(dbw), dpsm, dsax, dsay, rrw, rrh);
    }
    return can;
}

// Called by GS::performLocalToLocalTransfer after all graph/range barriers have completed. A false
// result leaves the caller on its unchanged legacy copy. A true result means the exact logical
// write lives in fbp=0x13b's FBO until the immediately measured G287 consumer or a fail-closed
// materialization edge.
bool g289TryGpuLocalCopy(uint8_t *vram, uint32_t vramSize,
                         uint32_t sbp, uint8_t sbw, uint8_t spsm,
                         uint32_t ssax, uint32_t ssay,
                         uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                         uint32_t dsax, uint32_t dsay,
                         uint32_t rrw, uint32_t rrh)
{
    uint32_t g345SrcFbp = 0xffffffffu;
    const bool g345Grab = g345GrabShape(sbp, sbw, spsm, ssax, ssay,
                                        dbp, dbw, dpsm, dsax, dsay, rrw, rrh, g345SrcFbp);
    if (g345Grab)
        g_g345GrabEdges.fetch_add(1u, std::memory_order_relaxed);
    if (!g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay,
                               dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
    {
        if (g345Grab)
        {
            // Aggregate reject-cause census (the G291 rule — never trust the capped early prints).
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
            if (!g289LocalCopyOn())
                g_g345RejOther.fetch_add(1u, std::memory_order_relaxed);
            else if (g345OwnerFor(dbp >> 5u) == nullptr && g345FreeSlot() == nullptr)
                g_g345RejOwnerBusy.fetch_add(1u, std::memory_order_relaxed);
            else if (!s_g276Pend.active)
                g_g345RejPendInactive.fetch_add(1u, std::memory_order_relaxed);
            else
                g_g345RejPendMismatch.fetch_add(1u, std::memory_order_relaxed);
        }
        return false;
    }

    // Source fbp: derived from sbp (grab-shaped whenever admission passed); the legacy pend
    // admission implies the same value it used before (blockOf(pend.fbp) == sbp).
    const uint32_t sourceFbp = g345Grab ? g345SrcFbp : s_g276Pend.fbp;
    const uint32_t destFbp = dbp >> 5u;
    if (!g289_backend_copy_display_to_work(sourceFbp, destFbp))
    {
        const uint64_t n =
            g_g289CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        std::fprintf(stderr,
                     "[G289:copy-fail] n=%llu src=0x%x dst=0x%x; legacy fallback\n",
                     static_cast<unsigned long long>(n), sourceFbp, destFbp);
        g276FlushPendingDisplay(8);
        if (g345Grab)
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    // G346 slot select: reuse the same-dst active owner (its whole range is superseded by this
    // identical-range copy) else take a free slot. Admission guaranteed one exists.
    G289CopyOwner *slot = g345OwnerFor(destFbp);
    const bool reused = slot != nullptr;
    if (slot == nullptr)
        slot = g345FreeSlot();
    if (slot == nullptr)
    {
        // Cannot represent ownership — fail closed: publish everything, let the CPU copy run.
        (void)g289MaterializeOwner(14);
        g276FlushPendingDisplay(8);
        if (g345Grab)
            g_g345GrabCpu.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const G144BlockRange ownerRange =
        g144RangeForRect(dbp, GS_PSM_CT32, 8u, 0u, 0u, 512u, 416u);
    // G346 owner-supersede (contract I1): this copy destructively overwrites every physical block
    // in ownerRange, so any OTHER active owner's claim on those blocks is dead. A superseded
    // PREFIX trims (the carried 0x14a owner vs a new 0x13b grab); full coverage deactivates; any
    // other overlap shape fails closed to materialization of that slot.
    for (uint32_t i = 0u; i < kG345OwnerSlots; ++i)
    {
        G289CopyOwner &o = s_g289Owners[i];
        if (!o.active || &o == slot)
            continue;
        if (o.firstOwnedBlock > o.lastOwnedBlock ||
            ownerRange.last < o.firstOwnedBlock || ownerRange.first > o.lastOwnedBlock)
            continue; // disjoint
        if (ownerRange.first <= o.firstOwnedBlock && ownerRange.last >= o.lastOwnedBlock)
        {
            o.active = false; // fully superseded
            g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (ownerRange.first <= o.firstOwnedBlock && ownerRange.last < o.lastOwnedBlock)
        {
            o.firstOwnedBlock = ownerRange.last + 1u; // prefix superseded, suffix survives
            g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            (void)g345MaterializeSlot(o, 15); // unsupported overlap shape — fail closed
        }
    }
    if (reused)
        g_g345Supersedes.fetch_add(1u, std::memory_order_relaxed);
    if (g345Grab)
    {
        g_g345GrabGpu.fetch_add(1u, std::memory_order_relaxed);
        if (slot != &s_g289Owners[0])
            g_g345Copies2nd.fetch_add(1u, std::memory_order_relaxed);
        if (!(s_g276Pend.active && s_g276Pend.fbp == sourceFbp))
            g_g345SnapAdmits.fetch_add(1u, std::memory_order_relaxed);
    }
    slot->active = true;
    ++slot->version;
    slot->srcFbp = sourceFbp;
    slot->dstFbp = destFbp;
    slot->firstOwnedBlock = ownerRange.first;
    slot->lastOwnedBlock = ownerRange.last;
    slot->vram = vram;
    slot->vramSize = vramSize;
    s_g289PendingUpload.active = false; // unconditional, exactly as the single-slot code did
    const uint64_t copyN =
        g_g289Copies.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g289StatOn() && (copyN <= 16u || (copyN % 256u) == 0u))
        std::fprintf(stderr,
                     "[G289:copy] n=%llu version=%llu src=0x%x dst=0x%x rows=0..415\n",
                     static_cast<unsigned long long>(copyN),
                     static_cast<unsigned long long>(slot->version), sourceFbp,
                     destFbp);

    if (g289VerifyOn())
    {
        // Make the legacy source bytes authoritative, materialize the GPU copy, then compare the
        // two exact CT32 transfer views pixel-for-pixel. This deliberately disables the payoff.
        g276FlushPendingDisplay(8);
        const bool matOk = g289MaterializeOwner(9);
        uint64_t bad = matOk ? 0u : 512u * 416u;
        const uint32_t srcBlock = framePageBaseToBlock(sourceFbp);
        const uint32_t dstBlock = framePageBaseToBlock(destFbp);
        if (matOk)
        {
            for (uint32_t y = 0u; y < 416u; ++y)
                for (uint32_t x = 0u; x < 512u; ++x)
                {
                    uint32_t srcPx = 0u, dstPx = 0u;
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(srcBlock, 8u, x, y);
                    const uint32_t dstOff = GSPSMCT32::addrPSMCT32(dstBlock, 8u, x, y);
                    if (srcOff + 4u > vramSize || dstOff + 4u > vramSize)
                    {
                        ++bad;
                        continue;
                    }
                    std::memcpy(&srcPx, vram + srcOff, 4u);
                    std::memcpy(&dstPx, vram + dstOff, 4u);
                    if (srcPx != dstPx)
                        ++bad;
                }
        }
        const uint64_t checks =
            g_g289VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g289VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (checks <= 16u || bad != 0u || (checks % 128u) == 0u)
            std::fprintf(stderr,
                         "[G289:verify] checks=%llu bad=%llu totalBad=%llu firstUse=%u\n",
                         static_cast<unsigned long long>(checks),
                         static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(
                             g_g289VerifyBad.load(std::memory_order_relaxed)),
                         checks == 1u ? 1u : 0u);
    }
    return true;
}

static bool g289CanConsumeOwner(uint8_t *vram, uint32_t vramSize)
{
    const G289CopyOwner *o13b = g345OwnerFor(0x13bu);
    if (o13b == nullptr || o13b->vram != vram ||
        o13b->vramSize != vramSize ||
        g_g144List.size() != 1u)
    {
        if (g345AnyOwnerActive() && g289StatOn())
        {
            static std::atomic<uint32_t> s_earlyRejectLogs{0};
            const uint32_t n = s_earlyRejectLogs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 8u)
                std::fprintf(
                    stderr,
                    "[G289:consume-early-reject] n=%u ownerVram=%p/%u vram=%p/%u entries=%zu\n",
                    n + 1u,
                    static_cast<void *>(o13b != nullptr ? o13b->vram : nullptr),
                    o13b != nullptr ? o13b->vramSize : 0u,
                    static_cast<void *>(vram), vramSize, g_g144List.size());
        }
        return false;
    }
    const G144Entry &e = g_g144List[0];
    const G144BlockRange tex = g144TextureRange(e);
    const G144BlockRange clut = g144ClutRange(e);
    const G144BlockRange owner = g289OwnerRange();
    const bool exact =
        e.ctx.frame.fbp == 0x13bu && e.ctx.frame.fbw == 8u &&
        e.ctx.frame.psm == GS_PSM_CT32 &&
        e.ctx.scissor.x0 == 0u && e.ctx.scissor.y0 == 0u &&
        e.ctx.scissor.x1 == 511u && e.ctx.scissor.y1 == 415u &&
        e.prim.type == GS_PRIM_SPRITE && e.prim.tme && !e.prim.abe &&
        e.ctx.tex0.tbp0 == 0x2720u && e.ctx.tex0.tbw == 2u &&
        e.ctx.tex0.psm == GS_PSM_T8 && e.ctx.tex0.tw == 7u && e.ctx.tex0.th == 7u &&
        e.ctx.tex0.cbp == 0x3fbcu && e.ctx.tex0.cpsm == GS_PSM_CT32 &&
        owner.valid && tex.valid && clut.valid &&
        !g144RangeOverlaps(owner, tex) && !g144RangeOverlaps(owner, clut);
    if (!exact && g289StatOn())
    {
        static std::atomic<uint32_t> s_rejectLogs{0};
        const uint32_t n = s_rejectLogs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 8u)
            std::fprintf(
                stderr,
                "[G289:consume-reject] n=%u frame=%03x/%u/%x sc=%u,%u..%u,%u "
                "prim=%u tme=%u abe=%u tex=%x/%u/%x/%u/%u clut=%x/%x "
                "range(owner=%x..%x tex=%x..%x clut=%x..%x)\n",
                n + 1u, e.ctx.frame.fbp, static_cast<unsigned>(e.ctx.frame.fbw),
                e.ctx.frame.psm, e.ctx.scissor.x0, e.ctx.scissor.y0,
                e.ctx.scissor.x1, e.ctx.scissor.y1, static_cast<unsigned>(e.prim.type),
                e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u, e.ctx.tex0.tbp0,
                static_cast<unsigned>(e.ctx.tex0.tbw), e.ctx.tex0.psm,
                static_cast<unsigned>(e.ctx.tex0.tw), static_cast<unsigned>(e.ctx.tex0.th),
                e.ctx.tex0.cbp, e.ctx.tex0.cpsm, owner.first, owner.last,
                tex.first, tex.last, clut.first, clut.last);
    }
    return exact;
}

// A versioned work-page owner may cross arbitrary native batches, not just the immediately next
// flush. Keep it GPU-local only when every color/depth/texture/CLUT range is known and disjoint;
// overlap or an unresolvable range fails closed to guest-VRAM materialization.
static bool g289BatchDisjointOwner(uint32_t &reason, size_t &hitEntry)
{
    const G144BlockRange owner = g289OwnerRange();
    reason = owner.valid ? 0u : 0x10u;
    hitEntry = 0u;
    bool overlap = false;
    bool unknown = !owner.valid;
    for (size_t i = 0u; i < g_g144List.size() && !overlap && !unknown; ++i)
    {
        const G144Entry &e = g_g144List[i];
        const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                              e.ctx.scissor.y1 >= e.ctx.scissor.y0;
        const G144BlockRange color = g144TargetRange(e);
        if (nonEmpty && !color.valid)
        {
            unknown = true;
            reason |= 0x11u;
        }
        else if (color.valid && g144RangeOverlaps(owner, color))
        {
            overlap = true;
            reason |= 0x1u;
        }

        const G254Surface depth = g254DepthSurface(e);
        if (depth.active && !depth.range.valid)
        {
            unknown = true;
            reason |= 0x12u;
        }
        else if (depth.active && g144RangeOverlaps(owner, depth.range))
        {
            overlap = true;
            reason |= 0x2u;
        }

        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                unknown = true;
                reason |= 0x14u;
            }
            else if (g144RangeOverlaps(owner, tex))
            {
                overlap = true;
                reason |= 0x4u;
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    unknown = true;
                    reason |= 0x18u;
                }
                else if (g144RangeOverlaps(owner, clut))
                {
                    overlap = true;
                    reason |= 0x8u;
                }
            }
        }
        if (overlap || unknown)
            hitEntry = i;
    }
    return !overlap && !unknown;
}

static bool g289OwnerAliasTuple(const G144Entry &e)
{
    const bool triangle =
        e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
        e.prim.type == GS_PRIM_TRIFAN;
    const bool format =
        e.ctx.tex0.psm == GS_PSM_CT32 || e.ctx.tex0.psm == GS_PSM_CT24;
    const bool ct24Alpha =
        e.ctx.tex0.psm != GS_PSM_CT24 ||
        (e.ctx.tex0.tcc == 1u && e.texa.ta0 == 128u &&
         !e.texa.aem && e.texa.ta1 == 128u);
    return triangle && e.prim.tme && e.prim.fst && format && ct24Alpha &&
           (e.ctx.tex0.tbp0 == 0x2720u || e.ctx.tex0.tbp0 == 0x2760u) &&
           e.ctx.tex0.tbw == 8u && e.ctx.tex0.tw == 9u && e.ctx.tex0.th == 9u;
}

// The measured final-composition batch may read the copied work pages through two CT32-page
// aliases. This coarse pass runs before classification and admits no bytes yet; it only defers
// fail-closed materialization until exact per-entry tap footprints are available.
static bool g289CanTryOwnerView(uint8_t *vram, uint32_t vramSize)
{
    const G289CopyOwner *o13b = g345OwnerFor(0x13bu);
    if (o13b == nullptr || o13b->vram != vram ||
        o13b->vramSize != vramSize || g_g144List.empty())
        return false;
    const uint32_t fbp = g_g144List[0].ctx.frame.fbp;
    if (fbp != 0u && fbp != 0x68u)
        return false;
    const G144BlockRange owner = g289OwnerRange();
    if (!owner.valid)
        return false;
    bool hit = false;
    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        if (!color.valid || g144RangeOverlaps(owner, color) ||
            (depth.active && (!depth.range.valid || g144RangeOverlaps(owner, depth.range))))
            return false;
        if (!e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        if (!tex.valid)
            return false;
        if (g144RangeOverlaps(owner, tex))
        {
            if (!g289OwnerAliasTuple(e))
                return false;
            hit = true;
        }
        if (g144IsPalettedPsm(e.ctx.tex0.psm))
        {
            const G144BlockRange clut = g144ClutRange(e);
            if (!clut.valid || g144RangeOverlaps(owner, clut))
                return false;
        }
    }
    return hit;
}

struct G289ConsumeGuard
{
    bool armed = false;
    ~G289ConsumeGuard()
    {
        if (armed && g345OwnerFor(0x13bu) != nullptr)
        {
            g_g289Unexpected.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(10);
        }
    }
};

struct G289OwnerViewGuard
{
    bool armed = false;
    ~G289OwnerViewGuard()
    {
        if (armed && g345OwnerFor(0x13bu) != nullptr)
        {
            g_g289OwnerViewFail.fetch_add(1u, std::memory_order_relaxed);
            (void)g289MaterializeOwner(11);
        }
    }
};

struct G289DirectGuard
{
    bool armed = false;
    ~G289DirectGuard()
    {
        if (armed && s_g276Pend.active)
            g276FlushPendingDisplay(0);
    }
};

static void g277CensusPrintMaybe()
{
    static uint64_t s_events = 0; // flush-owner thread only
    if ((++s_events % 4096u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G277:census] g276flush(c0=%llu c3=%llu c4=%llu c5=%llu c6=%llu c7=%llu c8=%llu) "
                 "inl(empty=%llu noovl=%llu ovl=%llu z=%llu) "
                 "e7(n=%llu skip=%llu) e8(n=%llu skip=%llu) "
                 "zrb(n=%llu coalescable=%llu)\n",
                 (unsigned long long)g_g277FlushByCause[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[6].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277FlushByCause[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlEmpty.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlNoOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlOvl.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277InlZ.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[7].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeCalls[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277EdgeSkippable[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRb.load(std::memory_order_relaxed),
                 (unsigned long long)g_g277ZRbCoalescable.load(std::memory_order_relaxed));
}

// Hard consumer edge (cause 3 CPU replay, 5 frame boundary, 6 un-ranged upload): always a real
// VRAM consumer — breaks the simulated Z-coalescing run.
static void g277CensusHardEdge(int cause)
{
    if (!g277CensusOn())
        return;
    if (cause >= 0 && cause < 9)
        g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    s_g277ZRunLive = false;
}

// Ranged transfer edge (cause 7 l2h dst, cause 8 l2l src+dst): skippable for the pending display
// flush when the transfer provably misses the pending rows; breaks the Z run only on Z overlap.
static void g277CensusRangeEdge(int cause, const G144BlockRange &src, const G144BlockRange &dst,
                                bool srcUnknown, bool dstUnknown)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[cause].fetch_add(1u, std::memory_order_relaxed);
    if (srcUnknown || dstUnknown)
    {
        s_g277ZRunLive = false;
        g277CensusPrintMaybe();
        return;
    }
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        if (pend.valid && !g144RangeOverlaps(pend, src) && !g144RangeOverlaps(pend, dst))
            g_g277EdgeSkippable[cause].fetch_add(1u, std::memory_order_relaxed);
    }
    if (s_g277ZRunLive &&
        (g144RangeOverlaps(s_g277ZRunRange, src) || g144RangeOverlaps(s_g277ZRunRange, dst)))
        s_g277ZRunLive = false;
    g277CensusPrintMaybe();
}

// Inline-primitive consumer edge (cause 4). Classifies by the prim's CLIPPED screen bbox: a fully
// scissored-out prim (the offscreen sceGsSwapDBuff sync linestrips, vtx x∈[1792,2303]) rasterizes
// nothing, reads nothing, writes nothing — a phantom consumer. Non-empty prims are classified by
// block-range overlap (target/Z/texture/CLUT vs the pending display rows), and break the simulated
// Z run only when they actually test/write Z.
static void g277CensusInlineEdge(const GSPrimReg &prim, const GSVertex *verts,
                                 const GSContext &ctx, const GSTexClutReg &texclut)
{
    if (!g277CensusOn())
        return;
    g_g277EdgeCalls[4].fetch_add(1u, std::memory_order_relaxed);
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x, minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x); maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y); maxYf = std::max(maxYf, verts[i].y);
    }
    const int bx0 = static_cast<int>(std::floor(minXf)) - ofx;
    const int bx1 = static_cast<int>(std::ceil(maxXf)) - ofx;
    const int by0 = static_cast<int>(std::floor(minYf)) - ofy;
    const int by1 = static_cast<int>(std::ceil(maxYf)) - ofy;
    const int cx0 = std::max(bx0, static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(bx1, static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(by0, static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(by1, static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
    {
        g_g277InlEmpty.fetch_add(1u, std::memory_order_relaxed); // phantom: draws nothing
        g277CensusPrintMaybe();
        return;
    }
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    const uint64_t ts = ctx.test;
    const bool zte = ((ts >> 16) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32) & 1u) != 0u;
    if ((zte && ztst >= 2u) || !zmsk)
    {
        g_g277InlZ.fetch_add(1u, std::memory_order_relaxed);
        if (s_g277ZRunLive)
        {
            const uint32_t zbpBlock = (static_cast<uint32_t>(ctx.zbuf) & 0x1FFu) << 5u;
            const G144BlockRange zr = g144RangeForRect(zbpBlock, GS_PSM_Z24, ctx.frame.fbw,
                                                       static_cast<uint32_t>(cx0),
                                                       static_cast<uint32_t>(cy0), w, h);
            if (!zr.valid || g144RangeOverlaps(s_g277ZRunRange, zr))
                s_g277ZRunLive = false;
        }
    }
    bool ovl = false;
    if (s_g276Pend.active)
    {
        const G144BlockRange pend = g277PendingDisplayRange();
        const uint32_t fbBlock = framePageBaseToBlock(ctx.frame.fbp);
        const G144BlockRange tgt = g144RangeForRect(fbBlock, ctx.frame.psm, ctx.frame.fbw,
                                                    static_cast<uint32_t>(cx0),
                                                    static_cast<uint32_t>(cy0), w, h);
        ovl = !pend.valid || !tgt.valid || g144RangeOverlaps(pend, tgt);
        if (!ovl && prim.tme)
        {
            const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
            const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
            const G144BlockRange tex =
                g144RangeForRect(ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw, 0u, 0u, texW, texH);
            ovl = !tex.valid || g144RangeOverlaps(pend, tex);
            if (!ovl && g144IsPalettedPsm(ctx.tex0.psm))
            {
                const uint32_t clutBw = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
                const G144BlockRange clut =
                    g144RangeForRect(ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                                     static_cast<uint32_t>(texclut.cou),
                                     static_cast<uint32_t>(texclut.cov), 16u, 16u);
                ovl = !clut.valid || g144RangeOverlaps(pend, clut);
            }
        }
        if (ovl)
            g_g277InlOvl.fetch_add(1u, std::memory_order_relaxed);
        else
            g_g277InlNoOvl.fetch_add(1u, std::memory_order_relaxed);
    }
    g277CensusPrintMaybe();
}

// ---- G278: within-drain depth-readback coalescing ----------------------------------------------
// A G262 write wave leaves its Z result in the per-key backend depth texture and extends this
// pending row union. Same-key waves render directly against that texture, without uploading stale
// VRAM. The union is published before the first actual VRAM-Z consumer and unconditionally at the
// drain/presentation boundary. This is deliberately NOT G242 ownership: no state crosses a frame.
static G144BlockRange g278PendingDepthRangeP(const G278PendDepth &p)
{
    if (!p.active || p.fbW <= 0 || p.rowHi < p.rowLo)
        return {};
    return g144RangeForRect(p.zbpPage << 5u, GS_PSM_Z24,
                            p.fbw, 0u,
                            static_cast<uint32_t>(p.rowLo),
                            static_cast<uint32_t>(p.fbW),
                            static_cast<uint32_t>(p.rowHi - p.rowLo + 1));
}

static G144BlockRange g278PendingDepthWholeRangeP(const G278PendDepth &p)
{
    if (!p.active || p.fbW <= 0)
        return {};
    return g144RangeForRect(p.zbpPage << 5u, GS_PSM_Z24,
                            p.fbw, 0u, 0u,
                            static_cast<uint32_t>(p.fbW), 512u);
}

static bool g278StaleRowsAny(const G278PendDepth &p)
{
    for (uint64_t bits : p.staleRows)
        if (bits != 0u)
            return true;
    return false;
}

static bool g278StaleRowsOverlapP(const G278PendDepth &p, int lo, int hi)
{
    if (!p.active || hi < lo)
        return false;
    lo = std::max(0, lo);
    hi = std::min(511, hi);
    for (int y = lo; y <= hi; ++y)
        if ((p.staleRows[static_cast<size_t>(y >> 6)] >>
             static_cast<uint32_t>(y & 63)) & 1u)
            return true;
    return false;
}

static void g278MarkStaleRowsForRangeP(G278PendDepth &p, const G144BlockRange &write)
{
    // G411 private depth has no guest-VRAM storage range. GS texture/upload writes to physical
    // ZBP=0xd0 pages are unrelated staging traffic and cannot make the private mirror stale.
    if (!p.active || p.privateMirror || !write.valid)
        return;
    const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
    if (!whole.valid || !g144RangeOverlaps(whole, write))
        return;
    const uint32_t firstBlock = std::max(whole.first, write.first);
    const uint32_t lastBlock = std::min(whole.last, write.last);
    const uint32_t firstPage = (firstBlock - whole.first) >> 5u;
    const uint32_t lastPage = (lastBlock - whole.first) >> 5u;
    const uint32_t pagesPerRow = std::max(1u, p.fbw);
    const int lo = static_cast<int>(firstPage / pagesPerRow) * 32;
    const int hi = std::min(
        511, static_cast<int>(lastPage / pagesPerRow) * 32 + 31);
    for (int y = lo; y <= hi; ++y)
        p.staleRows[static_cast<size_t>(y >> 6)] |=
            1ull << static_cast<uint32_t>(y & 63);
    g_g278StaleMarks.fetch_add(1u, std::memory_order_relaxed);
}

// Mark every active slot whose whole range the write touches (each slot clamps internally).
static void g278MarkStaleRowsForRange(const G144BlockRange &write)
{
    for (int s = 0; s < kG278PendSlots; ++s)
        if (s_g278PendTab[s].active)
            g278MarkStaleRowsForRangeP(s_g278PendTab[s], write);
}

static bool g278StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G278_STAT");
    return s_on;
}

static void g278Report(uint64_t flushes)
{
    if (!g278StatOn() || (flushes > 8u && (flushes % 256u) != 0u))
        return;
    std::fprintf(
        stderr,
        "[G278:zrb] deferred=%llu flushes=%llu saved=%llu rows=%llu uploadSkip=%llu "
        "fail=%llu inv=%llu stale=%llu reinit=%llu "
        "causes(0=%llu 1=%llu 3=%llu 4=%llu 5=%llu 6=%llu 7=%llu 8=%llu)\n",
        (unsigned long long)g_g278Deferred.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Flushes.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Saved.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushRows.load(std::memory_order_relaxed),
        (unsigned long long)g_g278UploadSkips.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278InvariantFail.load(std::memory_order_relaxed),
        (unsigned long long)g_g278StaleMarks.load(std::memory_order_relaxed),
        (unsigned long long)g_g278Reinits.load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[0].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[1].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[3].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[4].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[5].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[6].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[7].load(std::memory_order_relaxed),
        (unsigned long long)g_g278FlushByCause[8].load(std::memory_order_relaxed));
    if (g337ZAuthOn())
        std::fprintf(
            stderr,
            "[G337:zauth] carried=%llu evict=%llu ovlFlush=%llu wideDefer=%llu "
            "auditTex=%llu auditTarget=%llu auditXKey=%llu\n",
            (unsigned long long)g_g337Carried.load(std::memory_order_relaxed),
            (unsigned long long)g_g337SlotEvict.load(std::memory_order_relaxed),
            (unsigned long long)g_g337OvlFlush.load(std::memory_order_relaxed),
            (unsigned long long)g_g337WideDefer.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditTex.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditTarget.load(std::memory_order_relaxed),
            (unsigned long long)g_g337AuditCrossKey.load(std::memory_order_relaxed));
    if (g337ZAuthOn())
        std::fprintf(
            stderr,
            "[G337:direct] seeded=%llu tight=%llu noSkipRb=%llu other=%llu\n",
            (unsigned long long)g_g337DirSeeded.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirTight.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirNoSkipRb.load(std::memory_order_relaxed),
            (unsigned long long)g_g337DirOther.load(std::memory_order_relaxed));
}

static bool g278FlushPendSlot(G278PendDepth &slot, int cause)
{
    if (!slot.active)
        return true;
    const G278PendDepth p = slot;
    const int rows = p.rowHi - p.rowLo + 1;
    const int glY = 512 - 1 - p.rowHi;
    static std::vector<float> s_readback; // flush-owner thread only
    s_readback.clear();
    const auto t0 = g274ZTimeOn() ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    if (p.vram == nullptr || p.vramSize == 0u || p.fbW <= 0 || rows <= 0 ||
        !g242_backend_read_depth(p.key, p.fbW, 512, glY, rows, s_readback) ||
        s_readback.size() != static_cast<size_t>(p.fbW) * rows)
    {
        const uint64_t n = g_g278FlushFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
            std::fprintf(stderr,
                         "[G278:depth] PUBLISH FAIL n=%llu cause=%d key=0x%llx "
                         "zbp=0x%x rows=%d..%d fbW=%d\n",
                         (unsigned long long)n, cause,
                         (unsigned long long)p.key, p.zbpPage,
                         p.rowLo, p.rowHi, p.fbW);
        // Keep the pending owner armed so a transient backend failure can be retried before a
        // fallback/consumer proceeds. Any observed failure blocks promotion.
        return false;
    }

    constexpr double kDepthToZ = 4294967296.0;
    const uint32_t zbpBlock = p.zbpPage << 5u;
    const auto unpackDepthRows = [&](int laneLo, int laneHi) {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(p.rowHi - y) * p.fbW;
            for (int x = 0; x < p.fbW; ++x)
            {
                double scaled = static_cast<double>(
                                    s_readback[srcRow + static_cast<size_t>(x)]) *
                                kDepthToZ;
                if (scaled < 0.0) scaled = 0.0;
                if (scaled > 16777215.0) scaled = 16777215.0;
                if (p.privateMirror)
                    g403DisplayZWrite(x, y, 0u, static_cast<uint32_t>(scaled));
                else
                    GSMem::WriteZ24(p.vram, zbpBlock, p.fbw,
                                    static_cast<uint32_t>(x),
                                    static_cast<uint32_t>(y),
                                    static_cast<uint32_t>(scaled));
            }
        }
    };
    GSRowPool::instance().run(p.rowLo, p.rowHi,
                              rows >= 8 ? g275ZSwizzleLanes() : 1,
                              unpackDepthRows);
    if (!p.privateMirror)
    {
        g178BumpRectImpl(zbpBlock, p.fbw, GS_PSM_Z24, 0u,
                         static_cast<uint32_t>(p.rowLo),
                         static_cast<uint32_t>(p.fbW),
                         static_cast<uint32_t>(rows),
                         21u); // G582 source 21: alias-view depth publication
        g336NotePublishZ(zbpBlock, p.fbw, p.fbW, p.rowLo, p.rowHi);
        g149BumpVramGen();
    }
    if (g274ZTimeOn())
        g_g274ZRbNs.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count(),
            std::memory_order_relaxed);
    if (p.privateMirror)
    {
        // G274's equality stamp describes guest VRAM and is meaningless for the private mirror.
        g_g274LastZGen.erase(p.key);
        g590ZDropStamp(p.key); // G590 rides G274's stamp lifetime exactly
        // G591: but the private mirror now HAS an authority record of its own, and this deferred
        // publication is exactly the instant it becomes true — the unpack above just wrote the
        // resident texture's depth into the mirror over rows p.rowLo..p.rowHi.
        g591PrivZStamp(p.key, p.rowLo, p.rowHi);
    }
    else if (g278StaleRowsAny(p))
    {
        // An out-of-union CPU write left other backend rows stale. Force the next use to perform
        // the backend contract's full initialization; a whole-gen equality stamp would otherwise
        // let G274 incorrectly skip over those newer VRAM rows.
        g_g274LastZGen.erase(p.key);
        g_g262DepthInit.erase(p.key);
        g_g278Reinits.fetch_add(1u, std::memory_order_relaxed);
        g590ZDropStamp(p.key);
    }
    else if (g274ZTimeOn() || g274ZResidentOn() || g278DepthRbOn())
    {
        g_g274LastZGen[p.key] =
            g178GenSumRect(zbpBlock, p.fbw, GS_PSM_Z24, 0u, 0u,
                           static_cast<uint32_t>(p.fbW), 512u);
        // G590: snapshot the SAME instant per page. This publication just synced GPU->VRAM over
        // the whole surface, so the resident texture equals VRAM everywhere right now — the only
        // moment a per-window authority claim can be founded on.
        g590ZStamp(p.key, zbpBlock, p.fbw);
    }
    g_g262ZReadbacks.fetch_add(1u, std::memory_order_relaxed);
    if (p.privateMirror)
    {
        const uint64_t publishes =
            g_g411DepthPublishes.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g411DepthPublishRows.fetch_add(
            static_cast<uint64_t>(rows), std::memory_order_relaxed);
        if (g411SharedGpuDepthStatOn() &&
            (publishes <= 8u || (publishes % 128u) == 0u))
        {
            std::fprintf(
                stderr,
                "[G411:depth] batches=%llu uploadRows=%llu publishes=%llu publishRows=%llu "
                "last(cause=%d rows=%d..%d)\n",
                static_cast<unsigned long long>(
                    g_g411DepthBatches.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_g411DepthUploadRows.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(publishes),
                static_cast<unsigned long long>(
                    g_g411DepthPublishRows.load(std::memory_order_relaxed)),
                cause, p.rowLo, p.rowHi);
        }
    }
    if (p.display)
        g_g265ZReadbacks.fetch_add(1u, std::memory_order_relaxed);

    slot = {};
    slot.rowLo = 512;
    slot.rowHi = -1;
    const uint64_t n =
        g_g278Flushes.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g278FlushRows.fetch_add(static_cast<uint64_t>(rows),
                              std::memory_order_relaxed);
    if (cause >= 0 && cause < 9)
        g_g278FlushByCause[cause].fetch_add(1u, std::memory_order_relaxed);
    g278Report(n);
    return true;
}

// Conservative edge: publish EVERY pending owner (arm-off: only slot 0 is ever active, so this
// is byte-identical to the historical single-slot flush).
static bool g278FlushPendingDepth(int cause)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
        if (s_g278PendTab[s].active)
            ok = g278FlushPendSlot(s_g278PendTab[s], cause) && ok;
    return ok;
}

static bool g278FlushPendingDepthForRanges(int cause,
                                           const G144BlockRange &src,
                                           const G144BlockRange &dst,
                                           bool srcUnknown,
                                           bool dstUnknown)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
            continue; // private shared depth has no VRAM transfer consumer
        if (srcUnknown || dstUnknown)
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        const G144BlockRange dirty = g278PendingDepthRangeP(p);
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!dirty.valid || !whole.valid)
        {
            ok = g278FlushPendSlot(p, cause) && ok; // unknown owner window fails closed
            continue;
        }
        // Reads only require unpublished rows. A write that touches dirty rows must preserve the
        // historical GPU-Z-before-CPU-write order, so publish first. A write elsewhere in the same
        // target can proceed without forcing a readback: remember its rows as backend-stale and
        // force a full re-init when this owner ends (or publish before a same-key wave reaches
        // those rows).
        if ((src.valid && g144RangeOverlaps(dirty, src)) ||
            (dst.valid && g144RangeOverlaps(dirty, dst)))
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        if (dst.valid && g144RangeOverlaps(whole, dst))
            g278MarkStaleRowsForRangeP(p, dst);
    }
    return ok;
}

static bool g278FlushPendingDepthForDisplayOwners(int cause)
{
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
            continue; // display-color owners cannot consume the private Z mirror
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!whole.valid)
        {
            ok = g278FlushPendSlot(p, cause) && ok;
            continue;
        }
        bool flush = false;
        for (int di = 0; di < 2 && !flush; ++di)
        {
            uint32_t fbp = 0u, fbw = 0u;
            G144BlockRange dirty{};
            if (g272DirtyInfo(di, fbp, fbw, dirty) &&
                (!dirty.valid || g144RangeOverlaps(whole, dirty)))
                flush = true;
        }
        if (!flush && s_g276Pend.active)
        {
            const G144BlockRange display = g277PendingDisplayRange();
            if (!display.valid || g144RangeOverlaps(whole, display))
                flush = true;
        }
        if (flush)
            ok = g278FlushPendSlot(p, cause) && ok;
    }
    return ok;
}

// Exact inline edge: fully clipped primitives consume nothing. Otherwise publish only when the
// CPU primitive's target, active Z, texture, or CLUT range can touch unpublished Z bytes.
static bool g278FlushPendingDepthForInline(const GSPrimReg &prim,
                                           const GSVertex *verts,
                                           const GSContext &ctx,
                                           const GSTexClutReg &texclut)
{
    bool anyActive = false;
    for (int s = 0; s < kG278PendSlots && !anyActive; ++s)
        anyActive = s_g278PendTab[s].active;
    if (!anyActive)
        return true;
    int n = 3;
    switch (prim.type)
    {
    case GS_PRIM_POINT: n = 1; break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
    case GS_PRIM_SPRITE: n = 2; break;
    default: n = 3; break;
    }
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    float minXf = verts[0].x, maxXf = verts[0].x;
    float minYf = verts[0].y, maxYf = verts[0].y;
    for (int i = 1; i < n; ++i)
    {
        minXf = std::min(minXf, verts[i].x);
        maxXf = std::max(maxXf, verts[i].x);
        minYf = std::min(minYf, verts[i].y);
        maxYf = std::max(maxYf, verts[i].y);
    }
    const int cx0 = std::max(static_cast<int>(std::floor(minXf)) - ofx,
                             static_cast<int>(ctx.scissor.x0));
    const int cx1 = std::min(static_cast<int>(std::ceil(maxXf)) - ofx,
                             static_cast<int>(ctx.scissor.x1));
    const int cy0 = std::max(static_cast<int>(std::floor(minYf)) - ofy,
                             static_cast<int>(ctx.scissor.y0));
    const int cy1 = std::min(static_cast<int>(std::ceil(maxYf)) - ofy,
                             static_cast<int>(ctx.scissor.y1));
    if (cx1 < cx0 || cy1 < cy0)
        return true;

    const uint32_t x = static_cast<uint32_t>(cx0);
    const uint32_t y = static_cast<uint32_t>(cy0);
    const uint32_t w = static_cast<uint32_t>(cx1 - cx0 + 1);
    const uint32_t h = static_cast<uint32_t>(cy1 - cy0 + 1);
    // Build the primitive's consumed/produced ranges ONCE, then test each pending owner.
    const G144BlockRange target =
        g144RangeForRect(framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm,
                         ctx.frame.fbw, x, y, w, h);
    const uint64_t test = ctx.test;
    const bool zte = ((test >> 16u) & 1u) != 0u;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17u) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32u) & 1u) != 0u;
    const bool depthReads = zte && ztst >= 2u;
    const bool depthWrites = !zmsk;
    G144BlockRange zr{};
    uint64_t inlineDepthKey = 0u;
    bool inlinePrivateDepth = false;
    if (depthReads || depthWrites)
    {
        const uint32_t zbpPage =
            static_cast<uint32_t>(ctx.zbuf) & 0x1FFu;
        const uint32_t zbpBlock = zbpPage << 5u;
        const uint32_t zpsm =
            static_cast<uint32_t>((ctx.zbuf >> 24u) & 0xFu);
        inlineDepthKey = g242DepthKey(zbpPage, zpsm, ctx.frame.fbw);
        inlinePrivateDepth =
            g404SharedZScope(ctx.frame.fbp, ctx.frame.fbw, zbpPage, zpsm);
        zr = g144RangeForRect(zbpBlock, zpsm, ctx.frame.fbw, x, y, w, h);
    }
    G144BlockRange tex{}, clut{};
    bool texValidKnown = false, clutNeeded = false;
    if (prim.tme)
    {
        const uint32_t texW = 1u << std::min<uint8_t>(ctx.tex0.tw, 15u);
        const uint32_t texH = 1u << std::min<uint8_t>(ctx.tex0.th, 15u);
        tex = g144RangeForRect(ctx.tex0.tbp0, ctx.tex0.psm, ctx.tex0.tbw,
                               0u, 0u, texW, texH);
        texValidKnown = true;
        if (g144IsPalettedPsm(ctx.tex0.psm))
        {
            const uint32_t clutBw =
                texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            clut = g144RangeForRect(ctx.tex0.cbp, ctx.tex0.cpsm, clutBw,
                                    static_cast<uint32_t>(texclut.cou),
                                    static_cast<uint32_t>(texclut.cov), 16u, 16u);
            clutNeeded = true;
        }
    }
    bool ok = true;
    for (int s = 0; s < kG278PendSlots; ++s)
    {
        G278PendDepth &p = s_g278PendTab[s];
        if (!p.active)
            continue;
        if (p.privateMirror)
        {
            // Physical color/texture/CLUT ranges cannot consume the private mirror. An inline CPU
            // primitive using the same logical Z surface can, so publish at that semantic edge.
            if (inlinePrivateDepth && p.key == inlineDepthKey)
                ok = g278FlushPendSlot(p, 4) && ok;
            continue;
        }
        const G144BlockRange dirty = g278PendingDepthRangeP(p);
        const G144BlockRange whole = g278PendingDepthWholeRangeP(p);
        if (!dirty.valid || !whole.valid)
        {
            ok = g278FlushPendSlot(p, 4) && ok;
            continue;
        }
        const auto readsPending = [&](const G144BlockRange &r) {
            return !r.valid || g144RangeOverlaps(dirty, r);
        };
        const auto writesOwner = [&](const G144BlockRange &r) {
            return !r.valid || g144RangeOverlaps(whole, r);
        };
        bool mustPublish = writesOwner(target);
        if (!mustPublish && (depthReads || depthWrites))
            mustPublish = (depthReads && readsPending(zr)) ||
                          (depthWrites && writesOwner(zr));
        if (!mustPublish && texValidKnown)
        {
            mustPublish = readsPending(tex);
            if (!mustPublish && clutNeeded)
                mustPublish = readsPending(clut);
        }
        if (mustPublish)
            ok = g278FlushPendSlot(p, 4) && ok;
    }
    return ok;
}

// G263: exact GL-tap footprint admission for a BILINEAR FST sprite that samples a resident RTT
// target (the G262 census shape: the display composite, prim=6, CT32, tbp=0x2720, bilin=1 was the
// SOLE bind rejector). GL_LINEAR taps floor(s-0.5)/+1 around every sample; samples are the GL
// pixel-center evaluations of the pushed varyings (raw corner UVs, drawSprite's left-edge
// -0.5*gradient shift, linear in screen space) — the same 4-tap convention the CPU bilinear
// sampler applies (sampleU = texUf - 0.5, floor, +1), so GL and the CPU oracle read identical
// taps wherever both are defined. Admission requires every tap that can carry NONZERO weight to
// land on an FBO-DEFINED texel: rows [covLo..covHi], full width. One fold is allowed per axis:
// tap -1 under GS CLAMP folds onto texel 0 on both GL (CLAMP_TO_EDGE) and the CPU sampler, exact
// when row/col 0 is itself defined. Everything else fails closed to the materialize path:
// REPEAT consumers with edge taps (GL would clamp where the CPU wraps), taps past the covered
// rows (FBO content undefined there), taps past the DECLARED texture dims (the CPU folds at
// texW/texH, the fbW×512 FBO space does not).
// ===== G601 FIX (DEFAULT ON, rollback DC2_G601_NO_EMPTYBIND=1) =====
// A PRIMITIVE WITH NO COVERED PIXEL CENTRE HAS NO TAPS, SO THE ADMISSION CRITERION IS VACUOUSLY
// SATISFIED — NOT UNPROVABLE.
//
// `g263BilinearSpriteBind` admits a bilinear FST sprite when "every tap that can carry NONZERO
// weight lands on an FBO-defined texel". When an axis covers no GL pixel centre at all
// (`cL < cF`, or a zero-extent axis) there is no such tap, so the criterion holds for the empty
// set — yet the shipped code returned false and sent the entry to the fallback, whose first act is
// `g261Materialize(target)`. That publication clears `dirty`, and the `if (!r.dirty) continue;`
// gate at the top of the bind loop then refuses the FBO to **every later entry of the same batch**.
//
// Measured on `s05` (route10-s05-bandage, sf 6958..6985): the `0x13a` hair-band glow composite is
// 51 bilinear FST sprites reading tbp=0x2740. On sf 6963 / 6972 / 6981 — and only there — exactly
// ONE of them is sub-pixel on one axis (e.g. y = 1983.688..1984.000, cF=1984.50 > cL=1983.50).
// Its rejection materialized 0x13a with `cause=5 guarded=73`: the G536 page-ownership guard
// withheld the 73 pages `0x13a`'s window shares with sibling target `0x159` (whose own
// publication, two targets earlier in the same per-entry pass, had just moved their generations),
// so guest VRAM kept BLACK where the band is while the FBO held it (`bandFboMean=57.05`). The
// other 50 sprites then decoded that black VRAM and Monica's hair band vanished for one frame.
//
// Admitting the empty primitive is exact: `s_g261FboSrc` is consumed only when the batch is built
// for the GPU backend, where a quad covering no pixel centre emits no fragment either way. A
// tap-bounds QUERY (`outTaps != nullptr`, G280's alias screen) has no footprint to report and
// still fails closed.
static std::atomic<uint64_t> g_g601EmptyAdmit{0};
static bool g601NoEmptyBind()
{
    static const bool off = envFlagEnabled("DC2_G601_NO_EMPTYBIND");
    return off;
}
static bool g601StatOn()
{
    static const bool on = envFlagEnabled("DC2_G601_STAT");
    return on;
}
static bool g263BilinearSpriteBind(const G144Entry &e, const G178EntryState &st,
                                   const G261Res &r, int *outTaps = nullptr,
                                   uint32_t g601Tag = 0u)
{
    const GSVertex &q0 = e.v[0];
    const GSVertex &q1 = e.v[1];
    const bool g601Rej = g601RejOn() && g601Tag == g601Fbp() && g601Window();
    int tapLo[2] = {0, 0}, tapHi[2] = {0, 0};
    for (int a = 0; a < 2; ++a)
    {
        const float p0 = a ? q0.y : q0.x, p1 = a ? q1.y : q1.x;
        const float t0 = static_cast<float>(a ? q0.v : q0.u) / 16.0f;
        const float t1 = static_cast<float>(a ? q1.v : q1.u) / 16.0f;
        const float lo = std::min(p0, p1), hi = std::max(p0, p1);
        // First/last GL pixel centers covered by [lo, hi). XYOFFSET is integer at this level
        // (ofx = reg >> 4), so subtracting it keeps the +0.5 center grid — evaluate in raw coords.
        const float cF = std::ceil(lo - 0.5f) + 0.5f;
        const float cL = std::ceil(hi - 0.5f) - 0.5f;
        if (!(cL >= cF) || p1 == p0)
        {
            // No GL pixel centre on this axis (or a zero-extent axis): the primitive has no tap
            // with nonzero weight, so admission is vacuous — see the G601 header above.
            if (g601Rej)
                std::fprintf(stderr,
                             "[G601:rej] sf=%u fbp=0x%x why=degenerate axis=%d p=%.3f..%.3f "
                             "t=%.3f..%.3f cF=%.2f cL=%.2f | xy0=(%.2f,%.2f) xy1=(%.2f,%.2f) "
                             "uv0=(%u,%u) uv1=(%u,%u)\n",
                             g_dc2ScriptFrame.load(std::memory_order_relaxed), g601Tag, a,
                             lo, hi, t0, t1, cF, cL, q0.x, q0.y, q1.x, q1.y,
                             q0.u, q0.v, q1.u, q1.v);
            if (outTaps != nullptr || g601NoEmptyBind())
                return false; // a tap-bounds QUERY has no footprint to report — fail closed
            const uint64_t n = g_g601EmptyAdmit.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (g601StatOn() && (n <= 8u || (n % 256u) == 0u))
                std::fprintf(stderr,
                             "[G601:empty] admit=%llu sf=%u fbp=0x%x axis=%d p=%.3f..%.3f "
                             "cF=%.2f cL=%.2f\n",
                             static_cast<unsigned long long>(n),
                             g_dc2ScriptFrame.load(std::memory_order_relaxed), g601Tag, a,
                             lo, hi, cF, cL);
            return true;
        }
        const float g = (t1 - t0) / (p1 - p0);
        const float shift = -0.5f * g; // drawSprite left-edge (0.0) bias, exactly as pushed
        const float sA = t0 + (cF - p0) * g + shift;
        const float sB = t0 + (cL - p0) * g + shift;
        const float sLo = std::min(sA, sB), sHi = std::max(sA, sB);
        tapLo[a] = static_cast<int>(std::floor(sLo - 0.5f - 0.001f));
        // ceil() keeps an exactly-integral edge in place (that +1 neighbour has weight 0); the
        // epsilon biases toward INCLUDING a tap, i.e. toward rejection, never toward admission.
        tapHi[a] = static_cast<int>(std::ceil(sHi - 0.5f + 0.001f));
    }
    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (tapLo[0] >= 0 || (clampU && tapLo[0] == -1)) && tapHi[0] <= maxCol;
    const bool vOk = (tapLo[1] >= r.covLo || (clampV && r.covLo == 0 && tapLo[1] == -1)) &&
                     tapHi[1] <= maxRow;
    if (g601Rej && !(uOk && vOk))
        std::fprintf(stderr,
                     "[G601:rej] sf=%u fbp=0x%x why=tap uOk=%u vOk=%u tapU=%d..%d tapV=%d..%d "
                     "maxCol=%d maxRow=%d cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u | "
                     "xy0=(%.2f,%.2f) xy1=(%.2f,%.2f) uv0=(%u,%u) uv1=(%u,%u)\n",
                     g_dc2ScriptFrame.load(std::memory_order_relaxed), g601Tag,
                     uOk ? 1u : 0u, vOk ? 1u : 0u, tapLo[0], tapHi[0], tapLo[1], tapHi[1],
                     maxCol, maxRow, r.covLo, r.covHi, r.fbW, st.texW, st.texH,
                     static_cast<unsigned>(st.wrapU), static_cast<unsigned>(st.wrapV),
                     q0.x, q0.y, q1.x, q1.y, q0.u, q0.v, q1.u, q1.v);
    if (!(uOk && vOk) && g262CensusOn())
    {
        static std::atomic<uint32_t> s_g263RejLog{0};
        if (s_g263RejLog.fetch_add(1u, std::memory_order_relaxed) < 16u)
            std::fprintf(stderr,
                         "[G263:rej] xy0=(%.2f,%.2f) xy1=(%.2f,%.2f) uv0=(%u,%u) uv1=(%u,%u) "
                         "tapU=%d..%d tapV=%d..%d cov=%d..%d fbW=%d tex=%dx%d wrap=%u%u\n",
                         q0.x, q0.y, q1.x, q1.y, q0.u, q0.v, q1.u, q1.v,
                         tapLo[0], tapHi[0], tapLo[1], tapHi[1], r.covLo, r.covHi, r.fbW,
                         st.texW, st.texH, static_cast<unsigned>(st.wrapU),
                         static_cast<unsigned>(st.wrapV));
    }
    if (uOk && vOk && outTaps != nullptr)
    {
        outTaps[0] = tapLo[0];
        outTaps[1] = tapHi[0];
        outTaps[2] = tapLo[1];
        outTaps[3] = tapHi[1];
    }
    return uOk && vOk;
}

// G267: conservative tap footprint for an affine FST triangle. Every interpolated UV inside a
// triangle lies inside the per-axis convex hull of its three vertex UVs, so expanding that hull
// to the two GL_LINEAR taps is a safe (possibly over-wide) bound. Unlike the sprite helper above,
// this does not try to exploit exclusive right/bottom endpoints. The returned GS block range is
// used to reject a direct bind whenever another dirty resident target could contribute any sampled
// byte. That alias screen is load-bearing: binding one producer FBO while silently ignoring a dirty
// sibling would make an internal residency oracle pass but corrupt final composition.
struct G267TriFootprint
{
    int tapLoU = 0, tapHiU = -1;
    int tapLoV = 0, tapHiV = -1;
    uint32_t sampleX0 = 0, sampleY0 = 0, sampleX1 = 0, sampleY1 = 0;
    G144BlockRange sampleRange{};
};

static bool g267BilinearTriFootprint(const G144Entry &e, const G178EntryState &st,
                                     const G261Res &r, G267TriFootprint &out)
{
    float uvLo[2] = {FLT_MAX, FLT_MAX};
    float uvHi[2] = {-FLT_MAX, -FLT_MAX};
    for (int k = 0; k < 3; ++k)
    {
        const float u = static_cast<float>(e.v[k].u) / 16.0f;
        const float v = static_cast<float>(e.v[k].v) / 16.0f;
        if (!std::isfinite(u) || !std::isfinite(v))
            return false;
        uvLo[0] = std::min(uvLo[0], u); uvHi[0] = std::max(uvHi[0], u);
        uvLo[1] = std::min(uvLo[1], v); uvHi[1] = std::max(uvHi[1], v);
    }

    // GL_LINEAR samples floor(coord - 0.5) and its +1 neighbour. Epsilons only widen the
    // footprint, biasing uncertain half ties toward rejection rather than unsafe admission.
    out.tapLoU = static_cast<int>(std::floor(uvLo[0] - 0.5f - 0.001f));
    out.tapHiU = static_cast<int>(std::ceil (uvHi[0] - 0.5f + 0.001f));
    out.tapLoV = static_cast<int>(std::floor(uvLo[1] - 0.5f - 0.001f));
    out.tapHiV = static_cast<int>(std::ceil (uvHi[1] - 0.5f + 0.001f));

    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(r.fbW, st.texW) - 1;
    const int maxRow = std::min(r.covHi, st.texH - 1);
    const bool uOk = (out.tapLoU >= 0 || (clampU && out.tapLoU == -1)) &&
                     out.tapHiU <= maxCol;
    const bool vOk = (out.tapLoV >= r.covLo ||
                      (clampV && r.covLo == 0 && out.tapLoV == -1)) &&
                     out.tapHiV <= maxRow;
    if (!(uOk && vOk))
        return false;

    const uint32_t x0 = static_cast<uint32_t>(std::max(0, out.tapLoU));
    const uint32_t y0 = static_cast<uint32_t>(std::max(0, out.tapLoV));
    const uint32_t x1 = static_cast<uint32_t>(out.tapHiU);
    const uint32_t y1 = static_cast<uint32_t>(out.tapHiV);
    out.sampleX0 = x0; out.sampleY0 = y0; out.sampleX1 = x1; out.sampleY1 = y1;
    out.sampleRange = g144RangeForRect(e.ctx.tex0.tbp0, e.ctx.tex0.psm, e.ctx.tex0.tbw,
                                       x0, y0, x1 - x0 + 1u, y1 - y0 + 1u);
    return out.sampleRange.valid;
}

// The generic G144BlockRange is intentionally conservative: it collapses a 2-D page footprint
// to one contiguous min/max interval. For G267's narrow U=0..16 strip over many page rows that
// interval falsely includes the seven unsampled CT32 page columns between each real column-0
// page. Enumerate the exact sampled CT32 pages before declaring a dirty sibling alias.
static bool g267SamplePagesOverlap(const G144Entry &e, const G267TriFootprint &fp,
                                   const G144BlockRange &resident)
{
    if (!resident.valid || e.ctx.tex0.psm != GS_PSM_CT32)
        return true; // fail closed for unknown range/format
    const uint32_t pageBw = std::max<uint32_t>(1u, e.ctx.tex0.tbw);
    const uint32_t xp0 = fp.sampleX0 / 64u, xp1 = fp.sampleX1 / 64u;
    const uint32_t yp0 = fp.sampleY0 / 32u, yp1 = fp.sampleY1 / 32u;
    for (uint32_t yp = yp0; yp <= yp1; ++yp)
    {
        for (uint32_t xp = xp0; xp <= xp1; ++xp)
        {
            const uint64_t first64 = static_cast<uint64_t>(e.ctx.tex0.tbp0) +
                                     (static_cast<uint64_t>(yp) * pageBw + xp) * 32u;
            if (first64 > 0x3FFFu)
                return true;
            const G144BlockRange page{static_cast<uint32_t>(first64),
                                      static_cast<uint32_t>(std::min<uint64_t>(first64 + 31u,
                                                                               0x3FFFu)),
                                      true};
            if (g144RangeOverlaps(page, resident))
                return true;
        }
    }
    return false;
}

// G309 (default-off): request the one-pass authoritative-page atlas composite. The exact
// verifier implies the mechanism so first-use validation needs only DC2_G309_VERIFY=1.
static bool g309Requested()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G309_NO_MULTI_SOURCE") ||
            envFlagDisabled("DC2_G309_MULTI_SOURCE"))
            return false;
        return envFlagEnabled("DC2_G309_MULTI_SOURCE") ||
               envFlagEnabled("DC2_G309_VERIFY");
    }();
    return s_on;
}

// G294 (default-off, DC2_G294_OWNER_TOKEN=1; kill DC2_G294_NO_OWNER_TOKEN=1): per-physical-page
// authoritative-owner token. Requesting it implies the whole G280/G281/G282/G283 view+fanout+epoch
// stack so a same-binary A/B needs one behavior lever. Declared free here (env-only, no gate deps)
// so the request chains below and g283AuthorityRequested can all reference it.
// G446: this predicate is consulted from g283AuthorityOn(), which the T8 map resolver and the
// target-page authority call PER PAGE / PER DRAW on the GS worker — and it did three uncached
// getenv() calls per invocation. The G446 host PC sampler measured getenv at 1.40% of ALL GS-worker
// samples with the worker at 98% occupancy and the frame equal to its span (gsslow 0.98), i.e. the
// pole thread was spending ~0.5 ms/f inside the Windows CRT environment lock. This is the exact
// G268 defect, recurring at a site G268 never swept. Caching is exact: DC2_* levers are
// process-constant in every harness (the whole tree's `static const bool` idiom assumes it) and
// envFlagEnabled/envFlagDisabled have no other side effect.
static bool g294OwnerTokenRequested()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G294_NO_OWNER_TOKEN") || envFlagDisabled("DC2_G294_OWNER_TOKEN"))
            return false;
        return envFlagEnabled("DC2_G294_OWNER_TOKEN");
    }();
    return s_on;
}
