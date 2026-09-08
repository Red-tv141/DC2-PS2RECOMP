// ==================================================================================================
// G714 — SAME-FORMAT LOCAL->LOCAL EXECUTED AGAINST GPU-RESIDENT RAW GS VRAM (front-end half).
//
// WHY, FROM THE CENSUS AND NOTHING ELSE (plans/phase-G714-fix-log.md §1):
//   * `[G432:l2ledge]`: **99.7%** of the local->local edge's blocking time is ONE call,
//     `g261MaterializeForRanges`. The graph drain fires ONCE in 98,400 edges; every other stage
//     is under 0.1%. So the edge is a PUBLICATION, not a flush.
//   * `[G343:shape]`: the copies are 100% same-PSM and 100% of the cost is PSMT8->PSMT8.
//   * The 764 rows/frame it publishes split **60.7% SOURCE-driven / 39.3% destination-driven**.
//     G627's built-and-unpromoted lane mirror is destination-only and reaches at most 23.5%.
//
// So the mechanism has to read the SOURCE off the GPU, which nothing in this tree did. This file
// resolves an eligible transfer into ONE command over the persistent raw GS VRAM image (G630's
// 2048x512 R32UI, row p == guest page p) and hands it to the GL worker:
//
//     guest-authoritative pages -> raw       (page memcpy; NO per-pixel CPU work)
//     pages a dirty target owns -> raw       (GPU->GPU compute; this is the new half)
//     raw -> raw                             (GPU->GPU compute; the copy itself)
//     raw -> the destination target's FBO    (GPU->GPU compute)
//
// ⭐ THE INVARIANT (§4 of the brief): an admitted transfer never calls `g242PrepareVramReadRect`,
// never calls `g261Materialize`, and never touches one byte of guest VRAM. Guest VRAM stays stale
// for the destination rows, and that debt is carried by the residency record exactly the way a
// G261 WAVE carries it — "VRAM did NOT change, so the page generations stay untouched"
// (rasterizer_vram_materialization.inc). This is the authority change §5 asks for, and it is the
// one the shipped model already knows how to unwind.
//
// EXACTNESS. Every address comes from the SHIPPED per-PSM helpers (G594's law). The GPU form is a
// CPU-PROBED permutation table, and `g714ProbeGeom` does not merely build it — it VERIFIES, over a
// multi-page sample, that
//     addr(bp, bw, x, y) == bp*256 + ((y/ph)*pagesPerRow + (x/pw))*8192 + fwd[(y%ph)*pw + (x%pw)]
// and that `fwd` is a bijection. A PSM that fails either check is refused for the run.
//
//   DC2_G714_GPUL2L=1   arm            DC2_G714_NO_GPUL2L=1  rollback once promoted
//   DC2_G714_STAT=1     `[G714:l2l]` admission by calls, pixels and AVOIDED published rows,
//                       with every refusal classified.
// ==================================================================================================

// ---- gates (namespace-scope: G619's law) --------------------------------------------------------
static const bool s_g714Arm = envFlagEnabled("DC2_G714_GPUL2L") &&
                              !envFlagEnabled("DC2_G714_NO_GPUL2L");
static const bool s_g714Stat = envFlagEnabled("DC2_G714_STAT");
// ===== G715 PUBLICATION BISECT ===================================================================
// `DC2_G715_EAGER` proved the GPU transform and its source ingestion are EXACT: with it set, the
// armed binary is bit-identical to the control on all 87 script-clock-keyed `s05` frames, while the
// same binary without it reads p50=2.75. So the defect is not the copy — it is what the normal path
// declines to PUBLISH afterwards. EAGER does two separable things, and only one of them can ship:
//
//   MAT — `g261Materialize` the touched targets. Pushes the FBO's rows into guest VRAM. This is a
//         synchronous readback: it hands back the entire prize, so it can only ever be a diagnosis.
//   TAG — `g178NoteVramWriteRectTagged` on the destination rect, exactly as the shipped CPU body
//         AND the shipped `g289TryGpuLocalCopy` blit path both do (see `gpu_transfers_and_kick.inc`:
//         the tag runs ABOVE `if (g289GpuCopy) return;`, so even a pure FBO->FBO blit declares the
//         logical write). It is a generation bump plus a depth-overlap check — NO readback. G714 is
//         the only local->local path in the tree that skips it.
//
// TAG on its own would flip `g536GuestOwnsTargetPage` to true for the destination pages, which
// would make a later `g261Materialize` PRESERVE the stale guest bytes over the FBO's correct ones.
// So TAG mode re-anchors the written pages AFTER the bump: downstream cache validity moves, FBO
// authority does not.
// ⛔⛔⛔ AND BOTH HALVES ARE CONFOUNDED BY ADMISSION. `g261Materialize` CLEARS `dirty`, and
// `g714PageOwner` only calls a page FBO-owned while its target IS dirty — so every publishing arm
// shrinks its own population. Measured on `s05`, whole route, same binary:
//
//     normal  taken=83.67%  takenMpx=603.0      p50=2.75   <- the population that must be correct
//     TAG     taken=73.66%  takenMpx=412.3      p50=3.20
//     MAT     taken=19.75%  takenMpx= 14.3      p50=0.00   <- 42x fewer pixels than normal
//
// A publication arm that passes on 1/42 of the pixels has not shown the publication is what fixes
// it. `KEEP` restores `dirty/dirtyLo/dirtyHi` after the materialize, so the next edge is admitted
// exactly as in the normal arm and the population is preserved. Republishing rows already published
// is idempotent, so the only cost is speed — which these arms never gate on.
static const bool s_g715Eager = envFlagEnabled("DC2_G715_EAGER");
static const bool s_g715TagOnly = envFlagEnabled("DC2_G715_TAG");
static const bool s_g715MatOnly = envFlagEnabled("DC2_G715_MAT");
// Population-preserving publication arms.
static const bool s_g715MatKeep = envFlagEnabled("DC2_G715_MATKEEP");
// ⭐ The candidate SHIPPABLE half. `[G343:shape]` measured that 60.7% of the rows an l2l edge
// publishes are published because the SOURCE range overlaps a dirty target; only 39.3% are the
// destination's. Publishing just the destination keeps guest VRAM correct where the copy landed and
// still deletes the whole source materialization — the larger half, and the half nothing in this
// tree could reach before the GPU source ingest existed.
static const bool s_g715MatDst = envFlagEnabled("DC2_G715_MATDST");
// Consumer detector: fill the destination's guest pages with 0xA5 after an admitted copy. If any
// consumer reads guest VRAM for those pages before the next publication, it draws the poison.
static const bool s_g715Poison = envFlagEnabled("DC2_G715_POISON");
// In-situ identity bisect: the two FBO<->raw legs and nothing else (see the submit site).
static const bool s_g715IdentOnly = envFlagEnabled("DC2_G715_IDENTONLY");
// Bisect: drop the G536 whole-guest-page overlay from the ingest (see the patch loop).
static const bool s_g715NoPatch = envFlagEnabled("DC2_G715_NOPATCH");
// ⭐ Provenance bisect. Publish every touched target the shipped way, restore `dirty` so the
// population is unchanged, and then take EVERY source page out of guest VRAM instead of off the
// GPU. The copy, the write-back and the publication skip are untouched, so this arm answers exactly
// one question: are the bytes `g714In` puts in raw the bytes the CPU body would have read?
static const bool s_g715CpuSrc = envFlagEnabled("DC2_G715_CPUSRC");
// ⭐ The candidate architecture: GPU source ingest + GPU copy, destination published to GUEST VRAM
// (a narrow page read) instead of into the FBO, with the shipped tag carrying the authority.
static const bool s_g715VramDst = envFlagEnabled("DC2_G715_VRAMDST");
// Hold the admitted population fixed while the guest-VRAM placement is read on its own.
static const bool s_g715NoTag = envFlagEnabled("DC2_G715_NOTAG");
// Give the destination pages to guest VRAM in the residency record only — the protective half of
// the shipped tag, without the global page-generation bump that disturbs residency.
static const bool s_g715OwnGuest = envFlagEnabled("DC2_G715_OWNGUEST");
// Leave the CT32 edges to `g289TryGpuLocalCopy`, whose result lives in an FBO G714 never writes.
static const bool s_g715T8Only = envFlagEnabled("DC2_G715_T8ONLY");
// Perform the SOURCE depth publication the escape path performs and G714 skips.
static const bool s_g715SyncDepth = envFlagEnabled("DC2_G715_SYNCDEPTH");
// ⭐⭐⭐ THE END-TO-END ORACLE THAT WAS NEVER BUILT. Every oracle so far compares the mechanism
// against ITSELF: the copy oracle rebuilds its reference from the same packet and the same table,
// and the round-trip oracle compares raw against raw. Neither can see a disagreement between the
// GPU pipeline's OUTPUT and the bytes the shipped CPU body would have written. This one does:
// publish the source the way the CPU path would, read the GPU's destination back, and compare the
// two element by element at the shipped addresses. Diagnostic only — it always declines the
// transfer afterwards, so the CPU path runs and the image stays correct.
static const bool s_g715E2eVfy = envFlagEnabled("DC2_G715_E2EVFY");
// Decline every edge `g289TryGpuLocalCopy` would have taken (its result is an FBO G714 never writes).
static const bool s_g715Yield289 = envFlagEnabled("DC2_G715_YIELD289");

// ⭐⭐⭐ THE DIRTY-PERSISTENCE PROBE — NO GPU WORK AT ALL.
//
// The mask bisect exposed the real mechanism. Admission is SELF-AMPLIFYING: taking an edge skips
// the publication, so the target stays dirty, so the NEXT edge is admitted too. Measured on `s05`:
// unmasked takes 83.67%, but `ADMITMASK=0f` takes 15.51% and `f0` takes 8.55% — the halves do not
// sum, because every refusal lets the CPU path publish and CLEAR `dirty`.
//
// ⛔ And every arm this phase built "to preserve the population" — LATEPIN, MATKEEP, CPUSRC,
// E2EVFY — restored `dirty` by hand, which preserved exactly the state under suspicion. The one arm
// that let `dirty` clear (`MAT`) scored 0.00, and it was dismissed as a population artefact when the
// population change IS the mechanism.
//
// This arm settles it with no GPU work whatsoever: on an admitted edge, snapshot the residency's
// dirty interval, DECLINE (so the shipped CPU path performs the whole transfer and publishes), then
// restore the snapshot on the next call. Guest VRAM, the FBO, every publication and every byte are
// the shipped ones; the ONLY difference from a control run is that dirty spans stay open exactly as
// G714 leaves them. If this reproduces ~2.75, the defect is dirty persistence and nothing else.
static const bool s_g715HoldDirty = envFlagEnabled("DC2_G715_HOLDDIRTY");

// ⭐⭐⭐ THE CORRECTED ARCHITECTURE. `HOLDDIRTY` proved the defect is the dirty interval being left
// open, so the mechanism must CLOSE the span it declines to publish. It can, without a full-target
// readback, because the transfer's own bytes are the only thing those rows need:
//
//   1. GPU source ingest + GPU copy + raw->FBO write-back   (unchanged, proven exact)
//   2. read back ONLY the destination page span             (~1-2 pages, not 448 rows)
//   3. place exactly the transfer's elements in guest VRAM  (shipped addresses)
//   4. hand those pages to the guest in the residency record
//   5. NARROW the dirty interval by the rows just handled
//
// After (1)+(3) the FBO and guest VRAM AGREE for those rows, so there is no ownership ambiguity
// left to represent — which is exactly the state a publication would have produced, reached without
// `g261MaterializeForRanges`. The prize (99.7% of the edge) survives.
//
// ⛔ `dirtyLo..dirtyHi` is a single interval, so step 5 is exact only when the transfer's rows are a
// PREFIX, a SUFFIX or the WHOLE span. A strictly interior transfer would need two intervals, cannot
// be represented, and is REFUSED in admission (`interval`) — fail-closed, and the census prices it.
static const bool s_g715CloseDirty = envFlagEnabled("DC2_G715_CLOSEDIRTY");
static bool s_g715HoldPending = false;
static struct { bool dirty; int lo, hi; } s_g715HoldSave[kG248TargetCount];
// ⭐ Shape-bucket admission mask, for binary-searching WHICH transfers break the image when every
// individual mechanism has already been cleared. `DC2_G715_ADMITMASK=<hex>`; bit k admits bucket k.
// Default 0xff = every bucket, i.e. exactly the unmasked behaviour. Three runs localise a class.
static uint32_t g715AdmitMask()
{
    static const uint32_t m = []() -> uint32_t {
        const char *e = std::getenv("DC2_G715_ADMITMASK");
        if (e == nullptr || *e == '\0')
            return 0xffu;
        return static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) & 0xffu;
    }();
    return m;
}
static uint32_t g715ShapeBucket(uint8_t sbw, uint8_t dbw, uint32_t rrw, uint32_t rrh)
{
    return (static_cast<uint32_t>(sbw) * 7u + static_cast<uint32_t>(dbw) * 13u +
            rrw * 3u + rrh * 5u) & 7u;
}
// Bisects of the FRONT-END bookkeeping, the only code every failing arm shares and the clean
// full-population arm skips. NOANCHOR drops the G536 per-page re-anchor; NORESID drops that plus
// the derived-state invalidation (g455 / g291Cov / snapshot / g280 / g310).
static const bool s_g715NoAnchor = envFlagEnabled("DC2_G715_NOANCHOR");
static const bool s_g715NoResid = envFlagEnabled("DC2_G715_NORESID");
// ⭐ The publication the BARRIER skipped, replayed verbatim. `MATKEEP` published only the targets in
// `fboIn`; `g144L2lLatePin` replays `g261MaterializeForRanges(pin.src, pin.dst, srcUnknown,
// dstUnknown, …)`, which on an unknown range publishes EVERY dirty target — a strictly larger set.
// That difference is why MATKEEP is not a clean refutation of the publication skip.
static const bool s_g715LatePin = envFlagEnabled("DC2_G715_LATEPIN") ||
                                  envFlagEnabled("DC2_G715_PINSRC") ||
                                  envFlagEnabled("DC2_G715_PINDST");
// Which SIDE of the pin publication to replay: 1 = source range, 2 = destination range, 3 = both.
static const int s_g715PinSide = envFlagEnabled("DC2_G715_PINSRC") ? 1
                               : envFlagEnabled("DC2_G715_PINDST") ? 2
                                                                   : 3;
static const bool s_g715Mat = s_g715Eager || s_g715MatOnly || s_g715MatKeep;
static const bool s_g715Tag = s_g715Eager || s_g715TagOnly;
static const bool s_g715KeepDirty = s_g715MatKeep || s_g715MatDst;
static bool g714StatOn() { return s_g714Stat; }

// ---- per-PSM page geometry, probed and VERIFIED once --------------------------------------------
struct G714Geom
{
    uint32_t pw = 0u, ph = 0u, bpp = 0u;
    uint32_t bwProbe = 0u;      // the BITBLTBUF width that makes exactly one page per row
    uint32_t bwPageShift = 0u;  // pagesPerRow == max(1, bw >> bwPageShift)
    bool ok = false;
    std::vector<uint32_t> fwd;  // texel index -> byte offset inside its 8 KiB page
    std::vector<uint32_t> inv;  // element index (byteOffset / (bpp/8)) -> texel index
};

static uint32_t g714AddrFor(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm)
    {
    case GS_PSM_CT32: return GSPSMCT32::addrPSMCT32(bp, bw, x, y);
    case GS_PSM_T8:   return GSPSMT8::addrPSMT8(bp, bw, x, y);
    default:          return 0xFFFFFFFFu;
    }
}

static G714Geom g714ProbeGeom(uint8_t psm)
{
    G714Geom g;
    switch (psm)
    {
    case GS_PSM_CT32: g.pw = 64u;  g.ph = 32u; g.bpp = 32u; g.bwProbe = 1u; g.bwPageShift = 0u; break;
    case GS_PSM_T8:   g.pw = 128u; g.ph = 64u; g.bpp = 8u;  g.bwProbe = 2u; g.bwPageShift = 1u; break;
    default: return g; // every other PSM stays unsupported and is counted as a refusal
    }
    const uint32_t texels = g.pw * g.ph;
    const uint32_t step = g.bpp / 8u;
    if (texels * step != 8192u)
        return g;
    g.fwd.assign(texels, 0u);
    g.inv.assign(texels, 0u);
    std::vector<uint8_t> seen(texels, 0u);
    for (uint32_t y = 0; y < g.ph; ++y)
        for (uint32_t x = 0; x < g.pw; ++x)
        {
            const uint32_t off = g714AddrFor(psm, 0u, g.bwProbe, x, y);
            if (off >= 8192u || (off % step) != 0u)
                return g;
            const uint32_t t = y * g.pw + x;
            const uint32_t e = off / step;
            if (e >= texels || seen[e])
                return g; // not a bijection: refuse the PSM rather than guess
            seen[e] = 1u;
            g.fwd[t] = off;
            g.inv[e] = t;
        }
    // The page-decomposition identity the compute kernel assumes, checked against the shipped
    // address function over several pages, several page-aligned bases and both live widths.
    static const uint32_t kBps[] = {0u, 0x2720u, 0x3420u, 0x35e0u};
    for (uint32_t bpi = 0; bpi < 4u; ++bpi)
        for (uint32_t bw = g.bwProbe; bw <= 8u; bw += g.bwProbe)
        {
            const uint32_t bp = kBps[bpi];
            const uint32_t ppr = (bw >> g.bwPageShift) != 0u ? (bw >> g.bwPageShift) : 1u;
            for (uint32_t y = 0; y < g.ph * 3u; y += 7u)
                for (uint32_t x = 0; x < g.pw * ppr; x += 5u)
                {
                    const uint32_t want = g714AddrFor(psm, bp, bw, x, y);
                    const uint32_t got = bp * 256u +
                                         ((y / g.ph) * ppr + (x / g.pw)) * 8192u +
                                         g.fwd[(y % g.ph) * g.pw + (x % g.pw)];
                    if (want != got)
                        return g;
                }
        }
    g.ok = true;
    return g;
}

static const G714Geom &g714Geom(uint8_t psm)
{
    static const G714Geom ct32 = g714ProbeGeom(GS_PSM_CT32);
    static const G714Geom t8 = g714ProbeGeom(GS_PSM_T8);
    static const G714Geom none{};
    // The probe IS the addressing oracle: it fails closed on a non-bijective permutation or on any
    // disagreement with the shipped address helper's page decomposition. Report it once so a run's
    // log carries the evidence rather than the assumption.
    static const bool s_reported = [] {
        std::fprintf(stderr, "[G714:probe] ct32=%s(%ux%u/%ub) t8=%s(%ux%u/%ub)\n",
                     ct32.ok ? "PASS" : "REFUSED", ct32.pw, ct32.ph, ct32.bpp,
                     t8.ok ? "PASS" : "REFUSED", t8.pw, t8.ph, t8.bpp);
        return true;
    }();
    (void)s_reported;
    if (psm == GS_PSM_CT32) return ct32;
    if (psm == GS_PSM_T8) return t8;
    return none;
}

static uint32_t g714PagesPerRow(const G714Geom &g, uint32_t bw)
{
    const uint32_t ppr = bw >> g.bwPageShift;
    return ppr != 0u ? ppr : 1u;
}

// ---- census -------------------------------------------------------------------------------------
enum G714Rej : int
{
    kG714RejOff = 0, kG714RejPsm, kG714RejShape, kG714RejNoDirty, kG714RejOverlap,
    kG714RejPageOwner, kG714RejDepth, kG714RejStaleFbo, kG714RejDstOutsideDirty,
    kG714RejBounds, kG714RejSubmit, kG714RejBackend, kG714RejG289, kG714RejInterval,
    kG714RejCount
};
static const char *kG714RejName[kG714RejCount] = {
    "off", "psm", "shape", "nodirty", "overlap", "pageowner", "depth", "reconcile",
    "dstoutdirty", "bounds", "submit", "backend", "g289", "interval"};

struct G714Stat
{
    uint64_t calls = 0, taken = 0, takenPx = 0, escPx = 0;
    uint64_t rowsAvoided = 0;   // rows g261Materialize would have published for this edge
    uint64_t srcFboPages = 0, srcGuestPages = 0, dstPages = 0;
    uint64_t reverseX = 0, reverseY = 0;
    uint64_t rej[kG714RejCount] = {};
};
static G714Stat g_g714;

static void g714Report()
{
    const G714Stat &s = g_g714;
    const double c = static_cast<double>(s.calls ? s.calls : 1u);
    std::fprintf(stderr,
                 "[G714:l2l] calls=%llu taken=%llu (%.2f%%) takenMpx=%.2f rowsAvoided=%llu | "
                 "pages(srcFbo=%llu srcGuest=%llu dst=%llu)",
                 static_cast<unsigned long long>(s.calls),
                 static_cast<unsigned long long>(s.taken), 100.0 * s.taken / c,
                 static_cast<double>(s.takenPx) / 1.0e6,
                 static_cast<unsigned long long>(s.rowsAvoided),
                 static_cast<unsigned long long>(s.srcFboPages),
                 static_cast<unsigned long long>(s.srcGuestPages),
                 static_cast<unsigned long long>(s.dstPages));
    for (int i = 0; i < kG714RejCount; ++i)
        if (s.rej[i] != 0u)
            std::fprintf(stderr, " %s=%llu", kG714RejName[i],
                         static_cast<unsigned long long>(s.rej[i]));
    std::fprintf(stderr, " dirs(x=%llu y=%llu)\n",
                 static_cast<unsigned long long>(s.reverseX),
                 static_cast<unsigned long long>(s.reverseY));
    std::fflush(stderr);
}

static void g714NoteRej(int why)
{
    if (!g714StatOn())
        return;
    ++g_g714.rej[why];
    if ((++g_g714.calls % 20000ull) == 0ull)
        g714Report();
}

// ---- page ownership ------------------------------------------------------------------------------
// Who is authoritative for guest page `pg`?
//   kG714PageGuest   no dirty resident target holds it -> guest VRAM, travels as a page memcpy.
//   kG714PageFbo     exactly one target holds it, and its whole 32-row page band is INSIDE that
//                    target's dirty span -> the FBO is authoritative for every row of the page.
//   kG714PageRefuse  two dirty targets claim it (aliasing this phase does not arbitrate), or the
//                    band is only PARTIALLY dirty. A partially dirty band is the dangerous case:
//                    the FBO owns some of its rows and guest VRAM owns the rest, so NEITHER source
//                    is correct for the whole page and the page-granular ingest cannot be exact.
//   kG714PageStale   the target holds it, but its FBO is BEHIND guest VRAM for those rows (a G264
//                    upload mirror or a G280 overlay is outstanding). Refused, and counted apart
//                    from `pageowner` so the census can size the two causes separately.
enum G714PageOwn : int
{
    kG714PageGuest = 0, kG714PageFbo = 1, kG714PageRefuse = 2, kG714PageStale = 3
};

static G714PageOwn g714PageOwner(uint32_t pg, int &owner, int &bandLo, int &bandHi)
{
    owner = -1;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty || r.fbw == 0u || r.fbW <= 0 || r.dirtyHi < r.dirtyLo)
            continue;
        const uint32_t base = kG261Fbp[t];
        if (pg < base)
            continue;
        const uint32_t d = pg - base;
        const uint32_t pr = d / r.fbw;
        const uint32_t pc = d % r.fbw;
        if (pr >= 16u || pc >= r.fbw)
            continue;
        // ⛔ G715: the ingest/write-back dispatch covers x in [0, fbW), so a page whose columns run
        // PAST the target's pixel width is only partly ingested — its tail columns keep whatever
        // stale words the raw image happened to hold, and the copy can read them. The shipped
        // `g280DirtyTargetOwnsPage` applies exactly this test; `g714PageOwner` did not.
        if (static_cast<int>(pc + 1u) * 64 > r.fbW)
            return kG714PageRefuse;
        const int lo = static_cast<int>(pr) * 32;
        const int hi = lo + 31;
        if (hi < r.dirtyLo || lo > r.dirtyHi)
            continue; // inside the target's footprint but guest VRAM still owns those rows
        if (lo < r.dirtyLo || hi > r.dirtyHi)
            return kG714PageRefuse; // partially dirty band — see the note above
        // ⭐⭐⭐ "DIRTY" DOES NOT MEAN "THE FBO IS CURRENT" — but the answer is to RECONCILE, not to
        // refuse. Two shipped mechanisms leave the FBO behind guest VRAM for still-dirty rows
        // (G264's upload mirror, G280's alias overlays), and `g261Materialize` settles both before
        // its readback. `g714ReconcileTargets` runs exactly those two settlements before the
        // ingest; BOTH ARE VRAM->FBO UPLOADS, never `glReadPixels`, so the phase's invariant holds.
        // ⛔ Refusing instead was measured and is recorded in the log: row-scoped it still rejected
        // 87.6% of every edge that reached a dirty target, i.e. the whole population.
        if (owner >= 0)
            return kG714PageRefuse; // two dirty targets claim the same page
        owner = static_cast<int>(t);
        bandLo = lo;
        bandHi = hi;
    }
    return owner >= 0 ? kG714PageFbo : kG714PageGuest;
}

// The guest page range one FBO<->raw band covers. Two bands whose ranges intersect would make the
// ingest order decide the content of the shared pages, so the packet refuses that.
static void g714BandPages(uint32_t t, int rowLo, int rowHi, uint32_t &pgLo, uint32_t &pgHi)
{
    const G261Res &r = g_g261Res[t];
    const uint32_t base = kG261Fbp[t];
    const uint32_t prLo = static_cast<uint32_t>(std::max(0, rowLo)) / 32u;
    const uint32_t prHi = static_cast<uint32_t>(std::min(511, rowHi)) / 32u;
    pgLo = base + prLo * r.fbw;
    pgHi = base + prHi * r.fbw + (r.fbw - 1u);
}

// The exact byte span a rect occupies, walked page by page (page-aligned bases only, so the span
// is exactly [min, max] over the visited pages' fwd tables).
static void g714ByteSpan(const G714Geom &g, uint32_t bp, uint32_t ppr,
                         uint32_t sax, uint32_t say, uint32_t w, uint32_t h,
                         uint64_t &lo, uint64_t &hi)
{
    const uint32_t pgx0 = sax / g.pw, pgx1 = (sax + w - 1u) / g.pw;
    const uint32_t pgy0 = say / g.ph, pgy1 = (say + h - 1u) / g.ph;
    lo = static_cast<uint64_t>(bp) * 256ull +
         (static_cast<uint64_t>(pgy0) * ppr + pgx0) * 8192ull;
    hi = static_cast<uint64_t>(bp) * 256ull +
         (static_cast<uint64_t>(pgy1) * ppr + pgx1) * 8192ull + 8191ull;
}

// ---- the admission -------------------------------------------------------------------------------
// Pure. Called at the publication-skip site AND at the execute site, and both must agree, so it
// reads only state neither of them changes in between.
static bool g714CanTakeL2lImpl(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                               uint32_t ssax, uint32_t ssay,
                               uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                               uint32_t dsax, uint32_t dsay,
                               uint32_t rrw, uint32_t rrh,
                               bool note)
{
    if (s_g716Authority)
        return g716CanL2l(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
    if (!s_g714Arm)
        return false;
    if (!g261WaveOn() || dc2GetNativeScale() != 1)
    {
        if (note) g714NoteRej(kG714RejOff);
        return false;
    }
    if (spsm != dpsm || !g714Geom(static_cast<uint8_t>(spsm)).ok)
    {
        if (note) g714NoteRej(kG714RejPsm);
        return false;
    }
    // ⭐⭐⭐ G715: DO NOT STEAL `g289TryGpuLocalCopy`'S POPULATION.
    // `[G713:l2l]` measured 60,000 l2l calls as **8.5% taken by G289, 91.5% escaping**, and the
    // escape shape as `sameCT32=0.0% samePsmOther=100.0%` — so G289 owns the CT32 edges and the
    // PSMT8 edges are the ones that escape. G714's own `[G343:shape]` census agrees on the size:
    // CT32 is 6,044 of 68,608 edges (8.8%) and carries **0.0% of the materialisation cost**, which
    // is 100% PSMT8. But `g714Geom(CT32).ok` is true, so G714 has been admitting those edges and
    // taking them away from G289 — whose result is a GPU blit that "is versioned in fbp=0x13b's
    // FBO", a surface G714 never writes. Refusing them costs nothing and gives G289 its edges back.
    if (s_g715T8Only && spsm != GS_PSM_T8)
    {
        if (note) g714NoteRej(kG714RejPsm);
        return false;
    }
    if ((g715AdmitMask() & (1u << g715ShapeBucket(sbw, dbw, rrw, rrh))) == 0u)
    {
        if (note) g714NoteRej(kG714RejShape);
        return false;
    }
    // ⭐⭐⭐ G715: YIELD THE EDGES `g289TryGpuLocalCopy` WOULD HAVE TAKEN.
    //
    // `[G713:l2l]` measures **8.5% of local->local calls taken by G289**, and G289's result is a
    // GPU->GPU FBO blit into the WORK target — the call site's own words: *"exact logical write is
    // versioned in fbp=0x13b's FBO"*. G714 writes the destination's own FBO and guest VRAM, never
    // that one, so any edge it takes from G289 loses the blit entirely.
    //
    // ⛔ `DC2_G715_T8ONLY` did NOT give those edges back. It refused non-PSMT8, but G289's admission
    // is `g289CanDeferLocalCopy` — a display/pend-shape test that has nothing to do with the PSM, so
    // its population and the CT32 population are different sets. This is the direct test: ask G289's
    // own predicate and decline whatever it claims.
    //
    // Fail-closed and free: these edges keep the shipped path they have today, and the census names
    // the class so its size is measured rather than assumed.
    if (s_g715Yield289 &&
        g289CanDeferLocalCopy(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
    {
        if (note) g714NoteRej(kG714RejG289);
        return false;
    }
    const G714Geom &g = g714Geom(static_cast<uint8_t>(spsm));
    if (rrw == 0u || rrh == 0u || sbw == 0u || dbw == 0u ||
        (sbp & 31u) != 0u || (dbp & 31u) != 0u ||
        sbw < g.bwProbe || dbw < g.bwProbe)
    {
        if (note) g714NoteRej(kG714RejShape);
        return false;
    }
    if (!g714_backend_available())
    {
        if (note) g714NoteRej(kG714RejBackend);
        return false;
    }
    // An edge that publishes nothing is already free; taking it would only add GPU work.
    if (!g261AnyDirty())
    {
        if (note) g714NoteRej(kG714RejNoDirty);
        return false;
    }

    const uint32_t sppr = g714PagesPerRow(g, sbw);
    const uint32_t dppr = g714PagesPerRow(g, dbw);
    uint64_t sLo = 0, sHi = 0, dLo = 0, dHi = 0;
    g714ByteSpan(g, sbp, sppr, ssax, ssay, rrw, rrh, sLo, sHi);
    g714ByteSpan(g, dbp, dppr, dsax, dsay, rrw, rrh, dLo, dHi);
    const uint32_t vramSize = g_g261VramSize;
    if (vramSize == 0u || sHi >= vramSize || dHi >= vramSize ||
        sHi >= 4194304ull || dHi >= 4194304ull)
    {
        if (note) g714NoteRej(kG714RejBounds);
        return false;
    }
    // §9: the shipped CPU body copies per pixel in TRAVERSAL ORDER and therefore observes its own
    // partial writes when the rects alias. A parallel dispatch cannot reproduce that, so an
    // overlapping byte span is refused rather than approximated. Same rule G285 already applies.
    if (!(sHi < dLo || dHi < sLo))
    {
        if (note) g714NoteRej(kG714RejOverlap);
        return false;
    }
    // The THIRD staleness source, and the only one that stays a REFUSAL: `g242PrepareVramReadRect`
    // publishes any GPU-resident DEPTH surface aliasing the transfer's pages, and unlike the two
    // colour-side settlements that publication is a READBACK (`g242SyncDepthState`). Performing it
    // would reintroduce exactly the round trip this phase exists to delete, so a transfer whose
    // pages alias dirty GPU depth is declined and left to the shipped path.
    if (g242WriteOverlapsDirtyDepth(sbp, sbw, spsm, ssax, ssay, rrw, rrh) ||
        g242WriteOverlapsDirtyDepth(dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
    {
        if (note) g714NoteRej(kG714RejDepth);
        return false;
    }

    // Every DESTINATION page must be owned by exactly one dirty resident target, at rows already
    // inside its dirty span. Then the copy is a wave into rows the FBO already owns: the residency
    // record needs no new debt, and guest VRAM was already stale there.
    bool anyDstOwned = false;
    std::array<int, kG248TargetCount> admRowLo, admRowHi;
    admRowLo.fill(512);
    admRowHi.fill(-1);
    for (uint32_t pgy = dsay / g.ph; pgy <= (dsay + rrh - 1u) / g.ph; ++pgy)
        for (uint32_t pgx = dsax / g.pw; pgx <= (dsax + rrw - 1u) / g.pw; ++pgx)
        {
            const uint32_t pg = (dbp >> 5u) + pgy * dppr + pgx;
            int owner = -1, lo = 0, hi = 0;
            const G714PageOwn own = g714PageOwner(pg, owner, lo, hi);
            if (own == kG714PageRefuse || own == kG714PageStale)
            {
                if (note)
                    g714NoteRej(own == kG714PageStale ? kG714RejStaleFbo : kG714RejPageOwner);
                return false;
            }
            if (own == kG714PageGuest)
            {
                // The destination would have to reach guest VRAM, which only a readback can do.
                if (note) g714NoteRej(kG714RejDstOutsideDirty);
                return false;
            }
            anyDstOwned = true;
            if (owner >= 0)
            {
                admRowLo[owner] = std::min(admRowLo[owner], lo);
                admRowHi[owner] = std::max(admRowHi[owner], hi);
            }
        }
    if (!anyDstOwned)
    {
        if (note) g714NoteRej(kG714RejDstOutsideDirty);
        return false;
    }
    // ⭐ The interval constraint of the corrected mechanism (see `s_g715CloseDirty`). A transfer
    // strictly INSIDE a target's dirty span would split it in two, which `dirtyLo..dirtyHi` cannot
    // express, so it is declined here — where the decision is made and the state is still intact.
    if (s_g715CloseDirty)
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            if (admRowHi[t] < admRowLo[t])
                continue;
            const G261Res &r = g_g261Res[t];
            if (admRowLo[t] > r.dirtyLo && admRowHi[t] < r.dirtyHi)
            {
                if (note) g714NoteRej(kG714RejInterval);
                return false;
            }
        }
    // Every SOURCE page must be unambiguous too (owned by one target, or guest-authoritative).
    for (uint32_t pgy = ssay / g.ph; pgy <= (ssay + rrh - 1u) / g.ph; ++pgy)
        for (uint32_t pgx = ssax / g.pw; pgx <= (ssax + rrw - 1u) / g.pw; ++pgx)
        {
            const uint32_t pg = (sbp >> 5u) + pgy * sppr + pgx;
            int owner = -1, lo = 0, hi = 0;
            const G714PageOwn own = g714PageOwner(pg, owner, lo, hi);
            if (own == kG714PageRefuse || own == kG714PageStale)
            {
                if (note)
                    g714NoteRej(own == kG714PageStale ? kG714RejStaleFbo : kG714RejPageOwner);
                return false;
            }
        }
    return true;
}

// ---- execution -----------------------------------------------------------------------------------
struct G714Band { uint32_t fbp; int fbw, fbW, rowLo, rowHi; };

static void g714AddBand(std::vector<int32_t> &out, uint32_t t, int lo, int hi)
{
    const G261Res &r = g_g261Res[t];
    const int bandLo = lo & ~31;
    const int bandHi = hi | 31;
    // ⛔ MERGE ONLY WHAT TOUCHES. The first version merged every band of a target by min/max, so
    // two disjoint page rows (0..31 and 96..127) became one 0..127 span that CLAIMED rows the
    // target does not own here. Leg 2 then ingested the FBO for those rows over the top of a
    // guest-authoritative page leg 1 had just patched, and the copy read the wrong source bytes.
    // Adjacent or overlapping bands still coalesce; disjoint ones stay separate entries.
    for (size_t i = 0; i + 4 < out.size(); i += 5)
        if (out[i] == static_cast<int32_t>(kG261Fbp[t]) &&
            bandLo <= out[i + 4] + 1 && bandHi + 1 >= out[i + 3])
        {
            out[i + 3] = std::min(out[i + 3], bandLo);
            out[i + 4] = std::max(out[i + 4], bandHi);
            return;
        }
    out.push_back(static_cast<int32_t>(kG261Fbp[t]));
    out.push_back(static_cast<int32_t>(r.fbw));
    out.push_back(r.fbW);
    out.push_back(bandLo);
    out.push_back(std::min(511, bandHi));
}

static bool g714TryExecuteL2lImpl(uint8_t *vram, uint32_t vramSize,
                                  uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                  uint32_t ssax, uint32_t ssay,
                                  uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                  uint32_t dsax, uint32_t dsay,
                                  uint32_t rrw, uint32_t rrh,
                                  bool revX, bool revY)
{
    if (s_g716Authority)
        return g716ExecuteL2l(vram, vramSize, sbp, sbw, spsm, ssax, ssay,
                              dbp, dbw, dpsm, dsax, dsay, rrw, rrh, revX, revY);
    // The deferred half of the dirty-persistence probe: the previous admitted edge declined, the
    // shipped CPU path has since published and cleared `dirty`, and this restores it — reproducing
    // G714's dirty persistence with none of G714's other effects.
    if (s_g715HoldDirty && s_g715HoldPending)
    {
        s_g715HoldPending = false;
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            if (s_g715HoldSave[t].dirty)
            {
                G261Res &rr = g_g261Res[t];
                rr.dirty = true;
                rr.dirtyLo = std::min(rr.dirtyLo, s_g715HoldSave[t].lo);
                rr.dirtyHi = std::max(rr.dirtyHi, s_g715HoldSave[t].hi);
            }
    }
    if (vram == nullptr || vramSize == 0u ||
        !g714CanTakeL2lImpl(sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay,
                            rrw, rrh, false))
        return false;
    if (s_g715HoldDirty)
    {
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            s_g715HoldSave[t] = { g_g261Res[t].dirty, g_g261Res[t].dirtyLo, g_g261Res[t].dirtyHi };
        s_g715HoldPending = true;
        if (g714StatOn())
            g714NoteRej(kG714RejSubmit);
        return false; // the shipped path performs the transfer; only `dirty` is held open
    }

    const G714Geom &g = g714Geom(static_cast<uint8_t>(spsm));
    const uint32_t sppr = g714PagesPerRow(g, sbw);
    const uint32_t dppr = g714PagesPerRow(g, dbw);
    auto p = std::make_shared<G714L2lPacket>();

    // One-time tables. `g714PageWords` is the same exact 64x32 CT32 page word map G630 probes,
    // and the fwd/inv pair is this PSM's verified permutation.
    static bool s_mapSent = false;
    static uint8_t s_tableSent = 0xFFu;
    if (!s_mapSent)
    {
        const G714Geom &c = g714Geom(GS_PSM_CT32);
        if (!c.ok)
            return false;
        p->ct32PageWords.assign(64u * 32u, 0u);
        for (uint32_t i = 0; i < 64u * 32u; ++i)
            p->ct32PageWords[i] = c.fwd[i] >> 2u; // word offset inside the page
    }
    if (s_tableSent != spsm)
    {
        p->fwd = g.fwd;
        p->inv = g.inv;
        p->tableKey = spsm;
    }

    // ===== §7: settle the FBO GPU-SIDE before ingesting it =====
    // Every target that owns a page this transfer touches gets the two settlements
    // `g261Materialize` performs before ITS read — G264's deferred upload mirror and G280's alias
    // overlays. Both are VRAM->FBO uploads, so the source is still never materialised. Run BEFORE
    // the page walk below, because that walk is what decides which bytes come off the GPU.
    // A failure leaves the FBO not matching guest intent, so the transfer declines and G522's pin
    // replays the publication the barrier skipped.
    {
        uint32_t touched = 0u;
        const auto mark = [&](uint32_t pg) {
            int owner = -1, lo = 0, hi = 0;
            if (g714PageOwner(pg, owner, lo, hi) == kG714PageFbo && owner >= 0)
                touched |= 1u << owner;
        };
        for (uint32_t pgy = dsay / g.ph; pgy <= (dsay + rrh - 1u) / g.ph; ++pgy)
            for (uint32_t pgx = dsax / g.pw; pgx <= (dsax + rrw - 1u) / g.pw; ++pgx)
                mark((dbp >> 5u) + pgy * dppr + pgx);
        for (uint32_t pgy = ssay / g.ph; pgy <= (ssay + rrh - 1u) / g.ph; ++pgy)
            for (uint32_t pgx = ssax / g.pw; pgx <= (ssax + rrw - 1u) / g.pw; ++pgx)
                mark((sbp >> 5u) + pgy * sppr + pgx);
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            if ((touched & (1u << t)) == 0u)
                continue;
            if (!g280PrepareMaterialize(static_cast<int>(t)) ||
                !g264FlushMirror(static_cast<int>(t)))
            {
                if (g714StatOn())
                    g714NoteRej(kG714RejStaleFbo);
                return false;
            }
        }
    }

    // Classify every page the operation touches. A page a dirty target owns comes off the GPU;
    // everything else is guest-authoritative and travels as a straight 8 KiB page memcpy.
    std::vector<uint32_t> guestPages;
    std::array<int, kG248TargetCount> dstRowLo, dstRowHi;
    dstRowLo.fill(512);
    dstRowHi.fill(-1);
    std::vector<std::pair<uint32_t, uint32_t>> writtenPages; // target, target-local page
    uint32_t writtenTargets = 0u;
    const auto classify = [&](uint32_t pg, bool isDest) -> bool {
        int owner = -1, lo = 0, hi = 0;
        const G714PageOwn own = g714PageOwner(pg, owner, lo, hi);
        if (own == kG714PageRefuse || own == kG714PageStale)
            return false;
        if (own == kG714PageGuest)
        {
            if (isDest)
                return false; // admission already refused this; belt and braces
            guestPages.push_back(pg);
            return true;
        }
        // The page's CT32 rows are read into raw before the copy either way — for a destination
        // page so its untouched lanes survive, for a source page so the copy can read it.
        g714AddBand(p->fboIn, static_cast<uint32_t>(owner), lo, hi);
        if (isDest)
        {
            g714AddBand(p->fboOut, static_cast<uint32_t>(owner), lo, hi);
            writtenTargets |= 1u << owner;
            dstRowLo[owner] = std::min(dstRowLo[owner], lo);
            dstRowHi[owner] = std::max(dstRowHi[owner], hi);
            writtenPages.emplace_back(static_cast<uint32_t>(owner),
                                      pg - kG261Fbp[owner]);
        }
        return true;
    };

    const uint32_t dPgX0 = dsax / g.pw, dPgX1 = (dsax + rrw - 1u) / g.pw;
    const uint32_t dPgY0 = dsay / g.ph, dPgY1 = (dsay + rrh - 1u) / g.ph;
    for (uint32_t pgy = dPgY0; pgy <= dPgY1; ++pgy)
        for (uint32_t pgx = dPgX0; pgx <= dPgX1; ++pgx)
            if (!classify((dbp >> 5u) + pgy * dppr + pgx, true))
                return false;
    for (uint32_t pgy = ssay / g.ph; pgy <= (ssay + rrh - 1u) / g.ph; ++pgy)
        for (uint32_t pgx = ssax / g.pw; pgx <= (ssax + rrw - 1u) / g.pw; ++pgx)
            if (!classify((sbp >> 5u) + pgy * sppr + pgx, false))
                return false;

    // G715 CPUSRC: publish every touched target exactly as the shipped barrier would, keep the
    // admitted population by restoring `dirty`, then hand the copy the PUBLISHED guest bytes for
    // every source page. The patch list is applied after the ingest, so these override it.
    if (s_g715CpuSrc)
    {
        struct { bool dirty; int lo, hi; } sav[kG248TargetCount];
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            sav[t] = { g_g261Res[t].dirty, g_g261Res[t].dirtyLo, g_g261Res[t].dirtyHi };
        uint32_t done = 0u;
        for (size_t i = 0; i + 4 < p->fboIn.size(); i += 5)
        {
            const int ti = g248TargetIndex(static_cast<uint32_t>(p->fboIn[i]));
            if (ti >= 0 && (done & (1u << ti)) == 0u)
            {
                done |= 1u << ti;
                g261Materialize(ti, kG261MatLocal);
            }
        }
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            G261Res &rr = g_g261Res[t];
            rr.dirty = sav[t].dirty;
            rr.dirtyLo = sav[t].lo;
            rr.dirtyHi = sav[t].hi;
        }
        for (uint32_t pgy = ssay / g.ph; pgy <= (ssay + rrh - 1u) / g.ph; ++pgy)
            for (uint32_t pgx = ssax / g.pw; pgx <= (ssax + rrw - 1u) / g.pw; ++pgx)
                guestPages.push_back((sbp >> 5u) + pgy * sppr + pgx);
    }

    // ⭐ Two FBO->raw bands whose GUEST PAGE RANGES intersect would let ingest ORDER decide the
    // content of the shared pages, and the raw->FBO leg would then publish the loser's bytes into
    // the winner's surface. Two dirty targets that alias one page range is exactly the case
    // `g714PageOwner` refuses per page; this is the same rule for the full-width bands those pages
    // drag in. Cheap: there are at most a handful of bands.
    for (size_t a = 0; a + 4 < p->fboIn.size(); a += 5)
        for (size_t b = a + 5; b + 4 < p->fboIn.size(); b += 5)
        {
            const int ta = g248TargetIndex(static_cast<uint32_t>(p->fboIn[a]));
            const int tb = g248TargetIndex(static_cast<uint32_t>(p->fboIn[b]));
            if (ta < 0 || tb < 0)
                return false;
            uint32_t aLo = 0, aHi = 0, bLo = 0, bHi = 0;
            g714BandPages(static_cast<uint32_t>(ta), p->fboIn[a + 3], p->fboIn[a + 4], aLo, aHi);
            g714BandPages(static_cast<uint32_t>(tb), p->fboIn[b + 3], p->fboIn[b + 4], bLo, bHi);
            if (aLo <= bHi && bLo <= aHi)
                return false;
        }

    // G715: match the CPU publication's final source winner, including G536's page guard.
    // A dirty row can contain guest-owned pages. g261Materialize preserves those whole pages
    // AFTER its FBO readback; our equivalent is FBO ingestion followed by these guest patches.
    // Classify after mirror/overlay settlement, exactly where the publication's guard runs.
    // Include the full output bands so their untouched lanes also retain the guest's bytes.
    // `DC2_G715_NOPATCH=1` drops this guard (a bisect, not a fix): the patch overlays whole guest
    // pages on top of the ingest and is then written back into the FBO, which is a mutation the
    // shipped `g261Materialize` never performs — it preserves those pages in VRAM and leaves the
    // FBO alone. Same admitted population either way, so the two arms differ only in content.
    if (!s_g715NoPatch)
    for (size_t i = 0; i + 4 < p->fboIn.size(); i += 5)
    {
        const int ti = g248TargetIndex(static_cast<uint32_t>(p->fboIn[i]));
        if (ti < 0)
            return false;
        const G261Res &r = g_g261Res[ti];
        const uint32_t prLo = static_cast<uint32_t>(p->fboIn[i + 3]) / 32u;
        const uint32_t prHi = static_cast<uint32_t>(p->fboIn[i + 4]) / 32u;
        for (uint32_t py = prLo; py <= prHi; ++py)
            for (uint32_t px = 0; px < r.fbw; ++px)
                if (g536GuestOwnsTargetPage(r, kG261Fbp[ti], py, px))
                    guestPages.push_back(kG261Fbp[ti] + py * r.fbw + px);
    }

    // Guest pages travel verbatim: ascending, exactly 2048 words each (G631's coalescer contract).
    std::sort(guestPages.begin(), guestPages.end());
    guestPages.erase(std::unique(guestPages.begin(), guestPages.end()), guestPages.end());
    p->patchPages.reserve(guestPages.size());
    p->patchWords.reserve(guestPages.size() * 2048u);
    for (const uint32_t pg : guestPages)
    {
        const uint64_t byteBase = static_cast<uint64_t>(pg) * 8192ull;
        if (byteBase + 8192ull > vramSize)
            return false;
        p->patchPages.push_back(pg);
        const size_t at = p->patchWords.size();
        p->patchWords.resize(at + 2048u);
        std::memcpy(p->patchWords.data() + at, vram + byteBase, 8192u);
    }

    p->sbp = sbp;
    p->dbp = dbp;
    p->sPagesPerRow = sppr;
    p->dPagesPerRow = dppr;
    p->pw = g.pw;
    p->ph = g.ph;
    p->bpp = g.bpp;
    p->ssax = ssax; p->ssay = ssay;
    p->dsax = dsax; p->dsay = dsay;
    p->rrw = rrw;   p->rrh = rrh;
    p->revX = revX ? 1u : 0u;
    p->revY = revY ? 1u : 0u;
    p->dPgX0 = dPgX0; p->dPgY0 = dPgY0;
    p->dPgNx = dPgX1 - dPgX0 + 1u;
    p->dPgNy = dPgY1 - dPgY0 + 1u;

    // ⭐⭐⭐ G715 IN-SITU IDENTITY BISECT. Submit ONLY the two FBO<->raw legs and then decline the
    // transfer, so the shipped CPU body, its publication and G522's late pin all run exactly as in
    // the control. The single difference against an unarmed run is that every admitted band has
    // been read out of its FBO into raw and written straight back. `g714Out o g714In` is the
    // identity by construction, so ANY pixel movement under this arm is the FBO boundary itself —
    // measured at the full population, with the copy, the guest patch, the publication skip and the
    // residency bookkeeping all removed from the comparison.
    if (s_g715IdentOnly)
    {
        p->identOnly = 1u;
        p->patchPages.clear();
        p->patchWords.clear();
        (void)g714_backend_submit_l2l(p);
        if (g714StatOn())
            g714NoteRej(kG714RejSubmit);
        return false;
    }

    // ⭐⭐⭐ G715 VRAMDST — THE DESTINATION BELONGS IN GUEST VRAM, NOT IN THE FBO.
    //
    // `kG627PromotedDefaultOn == false`: the shipped renderer never patches an l2l destination back
    // into the resident CT32 surface. The CPU body writes guest VRAM, `g178NoteVramWriteRectTagged`
    // bumps those pages, and `g536GuestOwnsTargetPage` then makes guest VRAM the winner for exactly
    // those pages — the FBO keeps its own rendered colour there and is never published over them.
    // The two views of one address range (colour for the FBO, PSMT8 texels for the transfer) cannot
    // both live in a colour surface, and the residency model's answer is to give the pages to the
    // guest. So do that: read the destination pages back out of raw, place ONLY the transfer's own
    // bytes, and let the shipped tag carry the authority. The prize the phase is chasing — deleting
    // `g261MaterializeForRanges` at this edge, 99.7% of its cost — is untouched by this.
    std::vector<uint32_t> dstWords;
    if (s_g715E2eVfy)
    {
        const uint32_t dPg0 = (dbp >> 5u) + dPgY0 * dppr + dPgX0;
        const uint32_t dPg1 = (dbp >> 5u) + dPgY1 * dppr + dPgX1;
        if (dPg1 < dPg0 || dPg1 >= 512u)
            return false;
        p->dstPgLo = dPg0;
        p->dstPgCount = dPg1 - dPg0 + 1u;
        p->dstOut = &dstWords;
        p->noFboOut = 1u;
        if (!g714_backend_submit_l2l_sync(p) ||
            dstWords.size() != static_cast<size_t>(p->dstPgCount) * 2048u)
            return false;
        if (!p->ct32PageWords.empty())
            s_mapSent = true;
        if (p->tableKey != 0xFFFFFFFFu)
            s_tableSent = static_cast<uint8_t>(spsm);
        // Publish the source exactly as the escape path would, then restore `dirty` so the
        // admitted population is unchanged and the next edge sees the state it would have seen.
        {
            struct { bool dirty; int lo, hi; } sav[kG248TargetCount];
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
                sav[t] = { g_g261Res[t].dirty, g_g261Res[t].dirtyLo, g_g261Res[t].dirtyHi };
            uint32_t done = 0u;
            for (size_t i = 0; i + 4 < p->fboIn.size(); i += 5)
            {
                const int ti = g248TargetIndex(static_cast<uint32_t>(p->fboIn[i]));
                if (ti >= 0 && (done & (1u << ti)) == 0u)
                {
                    done |= 1u << ti;
                    g261Materialize(ti, kG261MatLocal);
                }
            }
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                G261Res &rr = g_g261Res[t];
                rr.dirty = sav[t].dirty;
                rr.dirtyLo = sav[t].lo;
                rr.dirtyHi = sav[t].hi;
            }
        }
        static std::atomic<uint64_t> s_runs{0}, s_checks{0}, s_bad{0}, s_reports{0};
        const uint32_t elemBytes = g.bpp / 8u;
        const uint64_t spanLo = static_cast<uint64_t>(p->dstPgLo) * 8192ull;
        const uint64_t spanHi = spanLo + static_cast<uint64_t>(p->dstPgCount) * 8192ull;
        const uint8_t *gpu = reinterpret_cast<const uint8_t *>(dstWords.data());
        uint64_t bad = 0u, checks = 0u;
        uint32_t firstX = 0u, firstY = 0u;
        for (uint32_t y = 0; y < rrh; ++y)
            for (uint32_t x = 0; x < rrw; ++x)
            {
                const uint64_t sOff = g714AddrFor(spsm, sbp, sbw, ssax + x, ssay + y);
                const uint64_t dOff = g714AddrFor(dpsm, dbp, dbw, dsax + x, dsay + y);
                if (sOff + elemBytes > vramSize || dOff < spanLo || dOff + elemBytes > spanHi)
                    continue;
                ++checks;
                if (std::memcmp(vram + sOff, gpu + static_cast<size_t>(dOff - spanLo),
                                elemBytes) != 0)
                {
                    if (bad == 0u) { firstX = x; firstY = y; }
                    ++bad;
                }
            }
        s_runs.fetch_add(1u, std::memory_order_relaxed);
        s_checks.fetch_add(checks, std::memory_order_relaxed);
        const uint64_t tot = s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
        if (bad != 0u && s_reports.fetch_add(1u, std::memory_order_relaxed) < 24u)
            std::fprintf(stderr,
                         "[G715:e2e-BAD] sbp=0x%x dbp=0x%x psm=0x%x sxy=(%u,%u) dxy=(%u,%u) "
                         "rr=%ux%u bad=%llu/%llu first=(%u,%u)\n",
                         sbp, dbp, static_cast<uint32_t>(spsm), ssax, ssay, dsax, dsay, rrw, rrh,
                         static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(checks), firstX, firstY);
        if ((s_runs.load(std::memory_order_relaxed) % 4096ull) == 0ull)
        {
            std::fprintf(stderr, "[G715:e2e] runs=%llu checks=%llu bad=%llu\n",
                         static_cast<unsigned long long>(s_runs.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(s_checks.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(tot));
            std::fflush(stderr);
        }
        if (g714StatOn())
            g714NoteRej(kG714RejSubmit);
        return false; // diagnostic only: the shipped path performs the transfer
    }
    if (s_g715VramDst || s_g715CloseDirty)
    {
        const uint32_t dPg0 = (dbp >> 5u) + dPgY0 * dppr + dPgX0;
        const uint32_t dPg1 = (dbp >> 5u) + dPgY1 * dppr + dPgX1;
        if (dPg1 < dPg0 || dPg1 >= 512u)
            return false;
        p->dstPgLo = dPg0;
        p->dstPgCount = dPg1 - dPg0 + 1u;
        p->dstOut = &dstWords;
        // ⭐ CLOSEDIRTY KEEPS the raw->FBO write-back. After it and the guest-VRAM write below, the
        // FBO and guest VRAM AGREE for these rows — which is what makes closing the interval honest.
        p->noFboOut = s_g715CloseDirty ? 0u : 1u;
    }

    const size_t nGuest = p->patchPages.size();
    const size_t nFboIn = p->fboIn.size() / 5u;
    const bool sentMap = !p->ct32PageWords.empty();
    const bool sentTab = p->tableKey != 0xFFFFFFFFu;
    if (s_g715VramDst || s_g715CloseDirty)
    {
        if (!g714_backend_submit_l2l_sync(p) ||
            dstWords.size() != static_cast<size_t>(p->dstPgCount) * 2048u)
        {
            if (g714StatOn())
                g714NoteRej(kG714RejSubmit);
            return false;
        }
        if (sentMap)
            s_mapSent = true;
        if (sentTab)
            s_tableSent = static_cast<uint8_t>(spsm);
        // Place EXACTLY the transfer's destination elements. A whole-page copy would carry the
        // ingested FBO over guest bytes outside the rect, which is the full-width publication this
        // phase exists to delete, in miniature.
        // ⭐ G594's law: the guest address comes from the SHIPPED helper
        // (`GSPSMT8::addrPSMT8` / `GSPSMCT32::addrPSMCT32` via `g714AddrFor`), never from a
        // re-derivation. `g714ProbeGeom` has already verified that the same value equals
        // `bp*256 + pageIndex*8192 + fwd[...]`, so the readback offset can be taken from it too —
        // and any disagreement between the two forms becomes impossible rather than merely unlikely.
        const uint8_t *src = reinterpret_cast<const uint8_t *>(dstWords.data());
        const uint32_t elemBytes = g.bpp / 8u;
        const uint64_t spanLo = static_cast<uint64_t>(p->dstPgLo) * 8192ull;
        const uint64_t spanHi = spanLo + static_cast<uint64_t>(p->dstPgCount) * 8192ull;
        for (uint32_t y = 0; y < rrh; ++y)
            for (uint32_t x = 0; x < rrw; ++x)
            {
                const uint64_t dstOff = g714AddrFor(dpsm, dbp, dbw, dsax + x, dsay + y);
                if (dstOff < spanLo || dstOff + elemBytes > spanHi ||
                    dstOff + elemBytes > vramSize)
                    return false;
                std::memcpy(vram + dstOff, src + static_cast<size_t>(dstOff - spanLo), elemBytes);
            }
        // ⭐⭐⭐ HAND THE DESTINATION PAGES TO THE GUEST — LOCALLY.
        //
        // `NOTAG` restored the population and then scored 2.76/3.19/3.37: the EXACT `NOOUT`
        // signature, i.e. the same image as writing nothing at all. The reason is that the bytes
        // were written and then thrown away — the destination rows are inside the target's dirty
        // span by admission, so the next `g261Materialize` republishes the FBO's PRE-COPY colour
        // over them, and without an ownership marking `g536CollectGuestOwnedPages` does not save
        // them. The shipped path is protected because its tag bumps `g_g178PageGen`.
        //
        // `g536GuestOwnsTargetPage` is literally `g_g178PageGen[page] != r.g536PageGen[p]`, so the
        // handover can be made **in the residency record alone**: un-anchor exactly the pages this
        // transfer wrote. That is the tag's protective half without its global generation bump —
        // which is the half that disturbs residency and collapses admission.
        if (s_g715OwnGuest)
            for (const auto &page : writtenPages)
            {
                G261Res &r = g_g261Res[page.first];
                if (r.g536PageGenFbw == r.fbw && page.second < r.g536PageGen.size())
                    r.g536PageGen[page.second] =
                        g_g178PageGen[kG261Fbp[page.first] + page.second]
                            .load(std::memory_order_relaxed) + 1ull;
            }
        // ⭐⭐⭐ STEP 5 — CLOSE THE INTERVAL THE PUBLICATION WOULD HAVE CLOSED.
        //
        // `HOLDDIRTY` proved that leaving `dirtyLo..dirtyHi` open is by itself sufficient to damage
        // the image (p90 1.11 / max 3.19 against a 0.00 floor, with NO GPU work and a byte-identical
        // guest VRAM and FBO). The rows this transfer handled now hold the same bytes in the FBO and
        // in guest VRAM, so they carry no debt and must leave the interval. Admission has already
        // refused the strictly-interior case, so the remainder is exactly representable.
        if (s_g715CloseDirty)
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                if ((writtenTargets & (1u << t)) == 0u)
                    continue;
                G261Res &r = g_g261Res[t];
                if (!r.dirty || r.dirtyHi < r.dirtyLo)
                    continue;
                const int lo = dstRowLo[t], hi = dstRowHi[t];
                if (hi < lo)
                    continue;
                if (lo <= r.dirtyLo && hi >= r.dirtyHi)
                {
                    r.dirty = false;
                    r.dirtyLo = 512;
                    r.dirtyHi = -1;
                }
                else if (lo <= r.dirtyLo)
                    r.dirtyLo = hi + 1;
                else if (hi >= r.dirtyHi)
                    r.dirtyHi = lo - 1;
                if (r.dirtyHi < r.dirtyLo)
                {
                    r.dirty = false;
                    r.dirtyLo = 512;
                    r.dirtyHi = -1;
                }
            }
        // ⛔ The full tag is CONFOUNDING: bumping the destination pages' generations changes what
        // `g536GuestOwnsTargetPage` answers on the NEXT edge, so admission moves with it
        // (83.67% -> 58.52%, `dstoutdirty` 1,743 -> 8,752). `DC2_G715_NOTAG=1` holds the population
        // fixed so the guest-VRAM placement can be read on its own.
        if (!s_g715NoTag)
        {
            extern void g178NoteVramWriteRectTagged(uint8_t *, uint32_t, uint32_t, uint32_t,
                                                    uint32_t, uint32_t, uint32_t, uint32_t,
                                                    uint32_t);
            g178NoteVramWriteRectTagged(vram, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, 8u);
        }
        // CLOSEDIRTY writes the FBO too (leg 4 ran), so the derived state that describes the FBO's
        // content must be invalidated exactly as on the main path.
        if (s_g715CloseDirty)
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                if ((writtenTargets & (1u << t)) == 0u)
                    continue;
                G261Res &r = g_g261Res[t];
                const int lo = dstRowLo[t];
                const int hi = std::min(511, dstRowHi[t]);
                g455NoteResidentWrite(static_cast<int>(t));
                if (g291AtlasOn() && r.g291CovFbw != 0u)
                    for (int y = lo; y <= hi && y < 512; ++y)
                        r.g291Cov[static_cast<size_t>(y)].fill(0u);
                G178FbSnap &snap = g_g178FbSnap[kG261Fbp[t]];
                snap.valid = false;
                snap.genSum = 0u;
                snap.rowLo = 1 << 30;
                snap.rowHi = -1;
                g280NoteFboContentChange(static_cast<int>(t), lo, hi);
                g310NoteProducerWrite(static_cast<int>(t), lo, hi);
            }
        if (g714StatOn())
        {
            G714Stat &s = g_g714;
            ++s.calls;
            ++s.taken;
            s.takenPx += static_cast<uint64_t>(rrw) * rrh;
            s.srcGuestPages += nGuest;
            s.srcFboPages += nFboIn;
            s.dstPages += static_cast<uint64_t>(p->dPgNx) * p->dPgNy;
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
                if (writtenTargets & (1u << t))
                {
                    const G261Res &r = g_g261Res[t];
                    if (r.dirtyHi >= r.dirtyLo)
                        s.rowsAvoided += static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
                }
            if ((s.calls % 20000ull) == 0ull)
                g714Report();
        }
        return true;
    }
    if (!g714_backend_submit_l2l(p))
    {
        if (g714StatOn())
            g714NoteRej(kG714RejSubmit);
        return false;
    }
    if (sentMap)
        s_mapSent = true;
    if (sentTab)
        s_tableSent = static_cast<uint8_t>(spsm);

    // These pages have absorbed their guest bytes and the local copy in FIFO order. A later
    // G536 publication must preserve the GPU result, not restore the pre-copy guest page.
    // Re-anchor only the pages actually written; unrelated guest writes remain outstanding.
    //
    // ⛔ `DC2_G715_NOANCHOR=1` bisects exactly this loop. It is the one write in the whole mechanism
    // that FORGIVES an unreconciled guest write — the hazard the comment 30 lines below names and
    // then declines to run for the window, while running it here per page. Every failing arm this
    // phase has scored executes it; the only clean full-population arm (`IDENTONLY`) returns before
    // it. `NORESID` additionally drops the derived-state invalidation that follows.
    if (!s_g715NoAnchor && !s_g715NoResid)
    for (const auto &page : writtenPages)
    {
        G261Res &r = g_g261Res[page.first];
        if (r.g536PageGenFbw == r.fbw && page.second < r.g536PageGen.size())
            r.g536PageGen[page.second] =
                g_g178PageGen[kG261Fbp[page.first] + page.second].load(std::memory_order_relaxed);
    }

    // ===== residency: this was a WAVE into rows the FBO already owned =====
    // Guest VRAM did not change, so no page generation moves (the G261 wave contract). The rows
    // were inside `dirtyLo..dirtyHi` by admission, so the debt interval is unchanged too — only
    // the derived GPU-side state that describes the FBO's content has to be invalidated.
    for (uint32_t t = 0; t < kG248TargetCount && !s_g715NoResid; ++t)
    {
        if ((writtenTargets & (1u << t)) == 0u)
            continue;
        G261Res &r = g_g261Res[t];
        const int lo = dstRowLo[t];
        const int hi = std::min(511, dstRowHi[t]);
        // ⭐ `covLo/covHi`, `dirtyLo/dirtyHi`, the claimed window and every page generation are
        // DELIBERATELY untouched. Admission guarantees the written rows were already inside the
        // dirty span, and dirty rows are covered rows by construction, so nothing about WHICH rows
        // the FBO owns has changed — only their content. Re-anchoring the window here would run
        // G597's hazard (forgiving a genuine unreconciled guest write on some other page of the
        // same target) for no gain.
        (void)r;
        g455NoteResidentWrite(static_cast<int>(t));
        if (g291AtlasOn() && r.g291CovFbw != 0u)
            for (int y = lo; y <= hi && y < 512; ++y)
                r.g291Cov[static_cast<size_t>(y)].fill(0u);
        // The FBO no longer equals the bytes the snapshot recorded for these rows.
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[t]];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        g280NoteFboContentChange(static_cast<int>(t), lo, hi);
        g310NoteProducerWrite(static_cast<int>(t), lo, hi);
    }

    if (g714StatOn())
    {
        G714Stat &s = g_g714;
        ++s.calls;
        ++s.taken;
        s.reverseX += revX ? 1u : 0u;
        s.reverseY += revY ? 1u : 0u;
        s.takenPx += static_cast<uint64_t>(rrw) * rrh;
        s.srcGuestPages += nGuest;
        s.srcFboPages += nFboIn;
        s.dstPages += static_cast<uint64_t>(p->dPgNx) * p->dPgNy;
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            if (writtenTargets & (1u << t))
            {
                const G261Res &r = g_g261Res[t];
                if (r.dirtyHi >= r.dirtyLo)
                    s.rowsAvoided += static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
            }
        if ((s.calls % 20000ull) == 0ull)
            g714Report();
    }
    // ⭐⭐⭐ The LAST member of the escape path never measured. G714 skips
    // `g242PrepareVramReadRect(SOURCE)` on the argument that admission already refuses any shape
    // where `g242WriteOverlapsDirtyDepth` is true — but that predicate is not the same function as
    // `g242SyncOverlappingDepth`, which is what actually publishes. Any case the guard calls clean
    // and the publisher would have acted on is a depth publication the shipped renderer performs
    // and G714 does not, and missing depth shows up exactly where the diff is: on the characters.
    if (s_g715SyncDepth)
    {
        extern void g242PrepareVramReadRect(uint8_t *, uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t, uint32_t);
        g242PrepareVramReadRect(vram, sbp, sbw, spsm, ssax, ssay, rrw, rrh);
    }
    if (s_g715LatePin)
    {
        // Replay the EXACT publication the barrier declined, then restore `dirty` so the admitted
        // population is unchanged. This is the faithful form of the MATKEEP arm.
        extern void g715LatePinPart(int which);
        struct { bool dirty; int lo, hi; } sav[kG248TargetCount];
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
            sav[t] = { g_g261Res[t].dirty, g_g261Res[t].dirtyLo, g_g261Res[t].dirtyHi };
        g715LatePinPart(s_g715PinSide);
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            G261Res &rr = g_g261Res[t];
            rr.dirty = sav[t].dirty;
            rr.dirtyLo = sav[t].lo;
            rr.dirtyHi = sav[t].hi;
        }
    }
    if (s_g715Mat || s_g715MatDst)
    {
        // Snapshot BEFORE the publication, restore after: `g261Materialize` clears `dirty`, and
        // `g714PageOwner` needs it set to keep calling these pages FBO-owned. Everything else the
        // publication updates (`covLo/covHi`, `genAtSkip`, `g536PageGen`, the mirror) is a correct
        // consequence of a real readback and is deliberately kept.
        struct { bool dirty; int lo, hi; } sav[kG248TargetCount];
        if (s_g715KeepDirty)
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
                sav[t] = { g_g261Res[t].dirty, g_g261Res[t].dirtyLo, g_g261Res[t].dirtyHi };
        uint32_t published = 0u;
        for (size_t i = 0; i + 4 < p->fboIn.size(); i += 5)
        {
            const int ti = g248TargetIndex(static_cast<uint32_t>(p->fboIn[i]));
            if (ti < 0 || (published & (1u << ti)) != 0u)
                continue;
            // MATDST publishes only where the copy LANDED. The source half of the publication —
            // 60.7% of the rows by `[G343:shape]` — is what the GPU ingest exists to delete.
            if (s_g715MatDst && !s_g715Mat && (writtenTargets & (1u << ti)) == 0u)
                continue;
            published |= 1u << ti;
            g261Materialize(ti, kG261MatLocal);
        }
        if (s_g715KeepDirty)
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                G261Res &r = g_g261Res[t];
                r.dirty = sav[t].dirty;
                r.dirtyLo = sav[t].lo;
                r.dirtyHi = sav[t].hi;
            }
    }
    if (s_g715Poison)
    {
        for (const auto &page : writtenPages)
        {
            const uint64_t at = static_cast<uint64_t>(kG261Fbp[page.first] + page.second) * 8192ull;
            if (at + 8192ull <= vramSize)
                std::memset(vram + at, 0xA5, 8192u);
        }
    }
    if (s_g715Tag)
    {
        extern void g178NoteVramWriteRectTagged(uint8_t *, uint32_t, uint32_t, uint32_t,
                                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        // G582 source 8: local-to-local copy destination — the same tag the CPU body uses.
        g178NoteVramWriteRectTagged(vram, dbp, dbw, dpsm, dsax, dsay, rrw, rrh, 8u);
        if (!s_g715Mat)
        {
            // The bump just told G536 the guest wrote these pages, and guest VRAM does NOT hold
            // the copy. Re-anchor so the FBO stays the authority for exactly the pages we wrote.
            for (const auto &page : writtenPages)
            {
                G261Res &r = g_g261Res[page.first];
                if (r.g536PageGenFbw == r.fbw && page.second < r.g536PageGen.size())
                    r.g536PageGen[page.second] =
                        g_g178PageGen[kG261Fbp[page.first] + page.second]
                            .load(std::memory_order_relaxed);
            }
        }
    }
    return true;
}
