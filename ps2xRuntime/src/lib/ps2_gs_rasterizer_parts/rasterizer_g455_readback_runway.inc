// ===== G455 exact colour-readback runway census (DIAGNOSIS ONLY) ================================
//
// DC2_G455_RUNWAY=1 measures whether a colour snapshot can be issued after the last native draw
// graph has been submitted, then consumed later at the existing publication boundary. It never
// changes submission, readback, ownership, or publication. Ownership-version checks reject an
// early issue if another writer appears, and row-window checks reject an undersized snapshot.

enum G455Site : int
{
    kG455Local139 = 0,
    kG455Resync139 = 1,
    kG455Display = 2,
    kG455SiteCount = 3,
};

struct G455Arm
{
    bool active = false;
    bool early = false;
    uint64_t writeSeq = 0u;
    uint32_t fbp = 0u;
    uint32_t fbw = 0u;
    int fbW = 0;
    int rowLo = 0;
    int rowHi = -1;
    std::chrono::steady_clock::time_point issued{};
};

struct G455Stat
{
    uint64_t issues = 0u, earlyIssues = 0u, consumes = 0u, earlyValid = 0u;
    uint64_t immediate = 0u, laterWriter = 0u, shapeFail = 0u, superseded = 0u;
    uint64_t blockedMirror = 0u, blockedOverlay = 0u, directFbo = 0u;
    uint64_t issuePx = 0u, consumePx = 0u;
    uint64_t runwayNs = 0u, runwayMinNs = ~0ull, runwayMaxNs = 0u;
    uint64_t readNs = 0u, readN = 0u;
};

struct G455Ticket
{
    int site = -1;
    std::chrono::steady_clock::time_point readStart{};
};

static G455Arm s_g455ResArm, s_g455DisplayArm;
static G455Stat s_g455Stat[kG455SiteCount];
static uint64_t s_g455ResWriteSeq = 0u, s_g455DisplayWriteSeq = 0u;
static uint64_t s_g455ReadCount = 0u;

static bool g455RunwayOn()
{
    static const bool on = envFlagEnabled("DC2_G455_RUNWAY");
    return on;
}

static void g455InvalidateForWriter(G455Arm &arm, G455Stat &st)
{
    if (arm.active)
    {
        ++st.laterWriter;
        arm.active = false;
    }
}

static void g455NoteResidentWrite(int ti)
{
    if (!g455RunwayOn() || ti < 0 || ti >= static_cast<int>(kG248TargetCount) ||
        kG261Fbp[ti] != 0x139u)
        return;
    g455InvalidateForWriter(s_g455ResArm, s_g455Stat[kG455Local139]);
    ++s_g455ResWriteSeq;
}

static void g455NoteDisplayWrite()
{
    if (!g455RunwayOn())
        return;
    g455InvalidateForWriter(s_g455DisplayArm, s_g455Stat[kG455Display]);
    ++s_g455DisplayWriteSeq;
}

static void g455SetArm(G455Arm &arm, G455Stat &st, uint64_t writeSeq,
                       uint32_t fbp, uint32_t fbw, int fbW, int rowLo, int rowHi,
                       bool early)
{
    if (arm.active)
        ++st.superseded;
    arm.active = true;
    arm.early = early;
    arm.writeSeq = writeSeq;
    arm.fbp = fbp;
    arm.fbw = fbw;
    arm.fbW = fbW;
    arm.rowLo = rowLo;
    arm.rowHi = rowHi;
    arm.issued = std::chrono::steady_clock::now();
    ++st.issues;
    if (early)
        ++st.earlyIssues;
}

// Called after the pending draw graph has executed, before the local-copy pin stages. From here to
// the transfer there is no draw submission on this thread; any later writer still invalidates arm.
static void g455ArmLocalEdge(const G144BlockRange &src, const G144BlockRange &dst,
                             bool srcUnknown, bool dstUnknown, bool displayWillDefer)
{
    if (!g455RunwayOn())
        return;
    for (int ti = 0; ti < static_cast<int>(kG248TargetCount); ++ti)
    {
        const G261Res &r = g_g261Res[ti];
        if (kG261Fbp[ti] != 0x139u || !r.dirty)
            continue;
        if (r.mirPending)
        {
            ++s_g455Stat[kG455Local139].blockedMirror;
            continue;
        }
        if (!g_g280Ovl[ti].empty())
        {
            ++s_g455Stat[kG455Local139].blockedOverlay;
            continue;
        }
        const bool hit = srcUnknown || dstUnknown || !r.range.valid ||
                         (src.valid && g144RangeOverlaps(r.range, src)) ||
                         (dst.valid && g144RangeOverlaps(r.range, dst));
        if (hit)
            g455SetArm(s_g455ResArm, s_g455Stat[kG455Local139], s_g455ResWriteSeq,
                       0x139u, r.fbw, r.fbW, r.dirtyLo, r.dirtyHi, true);
    }

    if (displayWillDefer && s_g276Pend.active)
        ++s_g455Stat[kG455Display].directFbo;
    if (!displayWillDefer && s_g276Pend.active && s_g276Pend.fbW > 0 &&
        s_g276Pend.rowHi >= s_g276Pend.rowLo)
    {
        const G144BlockRange pending = g144RangeForRect(
            s_g276Pend.fbBlock, GS_PSM_CT32, s_g276Pend.fbw, 0u,
            static_cast<uint32_t>(s_g276Pend.rowLo),
            static_cast<uint32_t>(s_g276Pend.fbW),
            static_cast<uint32_t>(s_g276Pend.rowHi - s_g276Pend.rowLo + 1));
        const bool hit = srcUnknown || dstUnknown || !pending.valid ||
                         (src.valid && g144RangeOverlaps(pending, src)) ||
                         (dst.valid && g144RangeOverlaps(pending, dst));
        if (hit)
            g455SetArm(s_g455DisplayArm, s_g455Stat[kG455Display],
                       s_g455DisplayWriteSeq, s_g276Pend.fbp, s_g276Pend.fbw,
                       s_g276Pend.fbW, s_g276Pend.rowLo, s_g276Pend.rowHi, true);
    }
}

#if defined(PS2X_G669_DIAG)
// ===== G669: the EARLY-ISSUE arm at the sites that actually carry the readback cost =============
//
// WHY THIS EXISTS. G668 read `[G455:runway] = 0.000/0.000/0.000 ms` on all three sites and all
// three routes and could not use it: that zero is a ZERO-SAMPLE ARTIFACT (Rule 38). `runwayNs`
// accumulates only under `if (arm.early)` (see g455BeginRead below), and `early = true` had exactly
// ONE producer — `g455ArmLocalEdge`, called from the local-COPY edge
// (rasterizer_draw_sprite.inc:3165) — which credits only kG455Local139 and kG455Display and was
// refused by `r.mirPending` on 98.1% of its opportunities (lavaboss 2,782/2,837; dungeon1 4,794;
// dragon 2,560). kG455Resync139 had NO early-arm site at all, so its `early = 0` restated the code
// rather than the run. The axis was unmeasured for the two sites that carry the cost:
// `[G279:mat]` puts `resync` at 0.877 ms/f and the display publication owns the frame's largest
// single read.
//
// ⭐ THE EARLIEST LEGAL ISSUE POINT IS NOT THE l2l EDGE. It is the instant a GPU batch's draws have
// landed in the persistent FBO and that batch's own readback was DEFERRED — which is exactly where
// the shipped code already calls g455NoteResidentWrite / g455NoteDisplayWrite to bump the write
// clock. From that instant the FBO holds the final content for those rows until the next writer,
// and the next writer bumps the SAME clock and invalidates the arm. So arming here is safe under
// exactly the contract `g455BeginRead` already enforces — `writeSeq` equality plus row containment
// (`arm.rowLo <= rowLo && arm.rowHi >= rowHi`) — and introduces no new invariant. A pending union
// that GROWS re-arms with the wider window, because the growth itself passes through the write
// clock first.
//
// THE MIRROR DEPENDENCY IS REAL, NOT AN ARTEFACT. `r.mirPending` means a G264 deferred VRAM→FBO
// upload is outstanding, i.e. guest VRAM holds bytes the FBO has not received
// (rasterizer_mat_support.inc:601), so a read issued before `g264FlushMirror` would return
// pre-upload content — which is precisely why `g455ArmLocalEdge` refuses. It is COUNTED per site
// here rather than assumed away. These two sites are reached after the flush entry's
// `g264FlushMirrorsAtFlush()` (rasterizer_vram_materialization.inc:264), so the common path finds
// the mirrors already drained; `mirrorBlocked` below reports whether that holds in practice.
//
// BEHAVIOUR-PURE: stores a timestamp and a row window, reads no GL state, issues no GL call and
// changes no decision. `#if`-excluded (rule 12c) because both call sites are in
// ps2_gs_rasterizer.cpp, the hottest TU.
static uint64_t g_g669Armed[kG455SiteCount] = {};
static uint64_t g_g669MirrorBlocked[kG455SiteCount] = {};
static uint64_t g_g669Superseded[kG455SiteCount] = {};
static uint64_t g_g669ShapeSkip[kG455SiteCount] = {};

static void g669ArmEarly(int site, G455Arm &arm, uint64_t writeSeq, uint32_t fbp, uint32_t fbw,
                         int fbW, int rowLo, int rowHi, bool mirrorPending)
{
    if (!g455RunwayOn() || site < 0 || site >= kG455SiteCount)
        return;
    if (fbW <= 0 || rowHi < rowLo || rowLo < 0 || rowHi >= 512)
    {
        ++g_g669ShapeSkip[site];
        return;
    }
    if (mirrorPending)
    {
        // A read issued now would observe pre-upload FBO content. Do NOT arm; report it.
        ++g_g669MirrorBlocked[site];
        return;
    }
    if (arm.active)
        ++g_g669Superseded[site];
    arm.active = true;
    arm.early = true;
    arm.writeSeq = writeSeq;
    arm.fbp = fbp;
    arm.fbw = fbw;
    arm.fbW = fbW;
    arm.rowLo = rowLo;
    arm.rowHi = rowHi;
    arm.issued = std::chrono::steady_clock::now();
    ++g_g669Armed[site];
}

// Called from the G261 wave tail, once this flush's rendered rows are unioned into the residency
// record and G280 has dropped the overlays those rows invalidate. ⚠️ `s_g455ResArm` is SHARED by
// kG455Local139 and kG455Resync139 (g455BeginRead picks the arm by site), so an arm placed here is
// credited to whichever of the two actually consumes it — which is the honest attribution. The
// `armed` counter below records where it was ISSUED.
static void g669ArmResidentEarly(int ti)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount) || kG261Fbp[ti] != 0x139u)
        return;
    const G261Res &r = g_g261Res[ti];
    if (!r.dirty)
        return;
    if (!g_g280Ovl[ti].empty())
    {
        ++s_g455Stat[kG455Resync139].blockedOverlay;
        return;
    }
    g669ArmEarly(kG455Resync139, s_g455ResArm, s_g455ResWriteSeq, kG261Fbp[ti], r.fbw, r.fbW,
                 r.dirtyLo, r.dirtyHi, r.mirPending);
}

// Called from the G276 coalesce tail, once the pending display union has absorbed this batch. The
// display FBO carries no G264 mirror (the mirror table is keyed on the G261 residency targets), so
// there is no upload dependency to clear at this site.
static void g669ArmDisplayEarly()
{
    if (!s_g276Pend.active)
        return;
    g669ArmEarly(kG455Display, s_g455DisplayArm, s_g455DisplayWriteSeq, s_g276Pend.fbp,
                 s_g276Pend.fbw, s_g276Pend.fbW, s_g276Pend.rowLo, s_g276Pend.rowHi, false);
}
#endif // PS2X_G669_DIAG

static G455Ticket g455BeginRead(int site, uint64_t writeSeq, uint32_t fbp, uint32_t fbw,
                                int fbW, int rowLo, int rowHi)
{
    if (!g455RunwayOn() || site < 0 || site >= kG455SiteCount)
        return {};
    G455Stat &st = s_g455Stat[site];
    G455Arm &arm = site == kG455Display ? s_g455DisplayArm : s_g455ResArm;
    const auto now = std::chrono::steady_clock::now();
    const bool shapeOk = arm.active && arm.writeSeq == writeSeq && arm.fbp == fbp &&
                         arm.fbw == fbw && arm.fbW == fbW &&
                         arm.rowLo <= rowLo && arm.rowHi >= rowHi;
    if (!shapeOk)
    {
        if (arm.active)
            ++st.shapeFail;
        g455SetArm(arm, st, writeSeq, fbp, fbw, fbW, rowLo, rowHi, false);
    }
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - arm.issued).count());
    ++st.consumes;
    if (arm.early)
    {
        ++st.earlyValid;
        st.issuePx += static_cast<uint64_t>(arm.fbW) * (arm.rowHi - arm.rowLo + 1);
        st.consumePx += static_cast<uint64_t>(fbW) * (rowHi - rowLo + 1);
        st.runwayNs += ns;
        st.runwayMinNs = std::min(st.runwayMinNs, ns);
        st.runwayMaxNs = std::max(st.runwayMaxNs, ns);
    }
    else
    {
        ++st.immediate;
    }
    arm.active = false;
    return {site, now};
}

static void g455Report()
{
    static const char *names[kG455SiteCount] = {"l2l139", "resync139", "display"};
    std::fprintf(stderr, "[G455:runway]");
    for (int i = 0; i < kG455SiteCount; ++i)
    {
        const G455Stat &s = s_g455Stat[i];
        std::fprintf(stderr,
                     " %s(issue=%llu early=%llu consume=%llu valid=%llu immediate=%llu "
                     "later=%llu shape=%llu super=%llu mirror=%llu overlay=%llu direct=%llu "
                     "runway=%.3f/%.3f/%.3fms "
                     "read=%.3fms px=%.2fx)",
                     names[i], static_cast<unsigned long long>(s.issues),
                     static_cast<unsigned long long>(s.earlyIssues),
                     static_cast<unsigned long long>(s.consumes),
                     static_cast<unsigned long long>(s.earlyValid),
                     static_cast<unsigned long long>(s.immediate),
                     static_cast<unsigned long long>(s.laterWriter),
                     static_cast<unsigned long long>(s.shapeFail),
                     static_cast<unsigned long long>(s.superseded),
                     static_cast<unsigned long long>(s.blockedMirror),
                     static_cast<unsigned long long>(s.blockedOverlay),
                     static_cast<unsigned long long>(s.directFbo),
                     s.earlyValid ? static_cast<double>(s.runwayNs) / 1.0e6 / s.earlyValid : 0.0,
                     s.runwayMinNs == ~0ull ? 0.0 : static_cast<double>(s.runwayMinNs) / 1.0e6,
                     static_cast<double>(s.runwayMaxNs) / 1.0e6,
                     s.readN ? static_cast<double>(s.readNs) / 1.0e6 / s.readN : 0.0,
                     s.consumePx ? static_cast<double>(s.issuePx) / s.consumePx : 0.0);
    }
    std::fprintf(stderr, "\n");
#if defined(PS2X_G669_DIAG)
    // G669: the early-issue arm's OWN bookkeeping, printed on its own line so it can never be
    // confused with the shipped G455 columns above. `armed` is where the arm was ISSUED;
    // `valid`/`runway` above are credited where it was CONSUMED. Read `armed` FIRST — a zero there
    // means the site never fired and every runway figure beside it is a zero-sample artifact
    // (Rule 38, which is the whole reason this instrument exists).
    std::fprintf(stderr, "[G669:early]");
    for (int i = 0; i < kG455SiteCount; ++i)
        std::fprintf(stderr, " %s(armed=%llu mirrorBlocked=%llu superseded=%llu shapeSkip=%llu)",
                     names[i], static_cast<unsigned long long>(g_g669Armed[i]),
                     static_cast<unsigned long long>(g_g669MirrorBlocked[i]),
                     static_cast<unsigned long long>(g_g669Superseded[i]),
                     static_cast<unsigned long long>(g_g669ShapeSkip[i]));
    std::fprintf(stderr, "\n");
#endif
}

static void g455EndRead(const G455Ticket &ticket)
{
    if (ticket.site < 0 || ticket.site >= kG455SiteCount)
        return;
    G455Stat &st = s_g455Stat[ticket.site];
    st.readNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - ticket.readStart).count());
    ++st.readN;
    if ((++s_g455ReadCount % 256u) == 0u)
        g455Report();
}
