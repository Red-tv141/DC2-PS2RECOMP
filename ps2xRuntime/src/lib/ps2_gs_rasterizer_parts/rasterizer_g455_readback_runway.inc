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
