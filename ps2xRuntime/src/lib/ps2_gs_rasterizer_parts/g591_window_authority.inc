// ===== G591: WINDOW-SCOPED SURFACE AUTHORITY ====================================================
//
// ONE defect, two surfaces: a WHOLE-SURFACE invalidation gating a WINDOWED repair.
//
//   A. DEPTH / the G411 private mirror.  G590 §3 measured `privMir = 100%` of the windowed
//      depth-upload population on BOTH heavy routes and `[G590:auth] zSkipRes = 0` on every
//      target: the mirror is a plain std::vector held OUTSIDE aliased VRAM, so it has no page
//      generations and therefore NO OWNERSHIP RECORD AT ALL.  G274's default-ON resident-depth
//      elision — believed to BE the depth authority model — is structurally inert.  Give the
//      mirror its own 16-band WRITE EPOCH, bumped at the per-draw predicate that already gates
//      every CPU access to it, stamped at the two GPU->mirror sync sites, and gate the windowed
//      depth upload on it.
//
//   B. COLOUR.  `snap.genSum` is a WHOLE-TARGET g178GenSumRect over all `kFbH` rows while the
//      upload it gates writes only rows `upLoEff..upHi`.  Ask the same question scoped to the
//      pages the batch's own window covers.
//
// WHY A PER-DRAW HOOK IS NOT "ENUMERATING THE WRITERS" (G589's stated failure mode).  The bump
// sits on `g403DisplayZ && <zWrite>` — the per-draw PREDICATE every g403DisplayZWrite in the
// sprite and triangle paths already sits behind — not on the write sites.  Any writer added
// inside a CPU draw is covered for free.  The only mirror writes outside a CPU draw are the two
// GPU->mirror readbacks, which are SYNCS and are excluded on purpose (they are the stamp sites).
// The residual risk is carried by a same-run oracle, not by the argument: DC2_G591_ZVERIFY builds
// the upload anyway on every skip and compares it against the resident texture, and it prints
// `checks=` beside `bad=` because a run that compared nothing must never look like one that
// passed (G589).
//
// FLAGS
//   DC2_G591_CENSUS=1   [G591:zwhy] + [G591:cwhy] — both censuses, taken BEFORE their gates, with
//                       every conjunct ranked (g513).  Behaviour-pure.
//   slice A is PROMOTED DEFAULT-ON            DC2_G591_NO_PRIVZ=1   rollback
//   DC2_G591_COL=1      arm slice B (REFUTED) DC2_G591_NO_COL=1     rollback
//   DC2_G591_ZVERIFY=1  same-run oracle for A (implies A; never valid for timing)
//   DC2_G591_CVERIFY=1  same-run oracle for B (implies B; never valid for timing)
//   DC2_G419_AB=privz | colwin   paired within-process arms (hold 16)

static bool g591CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G591_CENSUS");
    return on;
}
static bool g591ZVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G591_ZVERIFY");
    return on;
}
static bool g591CVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G591_CVERIFY");
    return on;
}
static bool g591PrivZPaired()
{
    static const bool paired = []() {
        const char *v = std::getenv("DC2_G419_AB");
        return v != nullptr && std::strcmp(v, "privz") == 0;
    }();
    return paired;
}
static bool g591ColPaired()
{
    static const bool paired = []() {
        const char *v = std::getenv("DC2_G419_AB");
        return v != nullptr && std::strcmp(v, "colwin") == 0;
    }();
    return paired;
}

// ENV-LEVEL selection, arm-INDEPENDENT.  Everything that must be symmetric between the two paired
// buckets — the epoch bump, the stamps, the census — keys off this, so the control arm maintains
// the identical state and only the SKIP differs (G520 §7b).
// G591 PROMOTION (2026-08-14): slice A is DEFAULT-ON.
//   exactness  4,471,128,064 depth values compared by DC2_G591_ZVERIFY across BOTH routes,
//              bad=0 readFail=0 (s05 2,835,349,504 / 13,312 checks; ridepod 1,635,778,560 /
//              7,680 checks) — and ridepod is the route where G590's shadow oracle measured the
//              windowed depth upload only 90.8–96.7% equal, i.e. where an unguarded skip is
//              provably WRONG and a missed writer would show;
//   payoff     ridepod paired 2/2, blocked −0.303 ms/f (t=−1.88) and −0.233 ms/f (t=−1.20),
//              97/140 windows favour the candidate (sign test z=4.56); s05 paired 2/2 NULL
//              (+0.070 / −0.104, |t|<0.5) — never a loss on the paired estimator;
//   engagement [G591:zwhy] CLEAN = 100% of the reachable population on s05 (bandMoved=0 over
//              122,368 decisions) and 30.7% on ridepod, where the epoch correctly DECLINES 28,270
//              batches whose mirror band moved.
// Rollback: DC2_G591_NO_PRIVZ=1.
static bool g591PrivZSelected()
{
    static const bool on = !envFlagEnabled("DC2_G591_NO_PRIVZ") &&
                           !envFlagDisabled("DC2_G591_PRIVZ");
    return on;
}
static bool g591ColSelected()
{
    static const bool on = (envFlagEnabled("DC2_G591_COL") || g591CVerifyOn() || g591ColPaired()) &&
                           !envFlagEnabled("DC2_G591_NO_COL");
    return on;
}

// The four same-run replay oracles (`g255RestoreCpuVerify`) SNAPSHOT AND RESTORE the private depth
// mirror from outside any draw, in a part that is included long before this one and therefore
// cannot call the epoch hook. Rather than leave a silent hole, this whole unit refuses to arm while
// any of them is set — they are diagnostic oracles that must never be combined with a behaviour
// lever anyway, and refusing here makes the exclusion structural instead of a comment.
static bool g591ReplayOracleArmed()
{
    static const bool on =
        envFlagEnabled("DC2_G570_VERIFY_13D") || envFlagEnabled("DC2_G583_VERIFY_TRN13B") ||
        envFlagEnabled("DC2_G586_VERIFY_TRN141") || envFlagEnabled("DC2_G587_VERIFY_TRN15D");
    return on;
}

// ⚠️ THE TWO SLICES NEED SEPARATE MAINTENANCE GATES. Slice A is promoted default-ON and slice B is
// REFUTED, so a single shared gate would run slice B's per-page stamp (128 page-gen loads at every
// one of the ~57 flushes per frame) in the shipped default to serve a lever that can never fire —
// dead work in exactly the false-promotion direction, and invisible to the A/B because both buckets
// would pay it.
static bool g591ZTracked()
{
    static const bool on = !g591ReplayOracleArmed() && (g591CensusOn() || g591PrivZSelected());
    return on;
}
static bool g591ColTracked()
{
    static const bool on = !g591ReplayOracleArmed() && (g591CensusOn() || g591ColSelected());
    return on;
}
// Kept for the shared predicates below; never use it to gate slice-specific maintenance.
static bool g591Tracked()
{
    static const bool on = g591ZTracked() || g591ColTracked();
    return on;
}

// Per-call: honours the paired arm, which flips every A/B tick and must never be cached.
static bool g591PrivZOn()
{
    if (!g591ZTracked() || !g591PrivZSelected())
        return false;
    if (g591PrivZPaired())
        return g591PrivZArm() > 0;
    return true;
}
static bool g591ColOn()
{
    if (!g591ColTracked() || !g591ColSelected())
        return false;
    if (g591ColPaired())
        return g591ColWinArm() > 0;
    return true;
}

// ===== SLICE A: the private depth mirror's WRITE EPOCH ==========================================
//
// The mirror (g403DisplayZBuf) is 512x512 and is shared by every ZBP 0xd0 / PSMZ24 / FBW=8 draw,
// so it maps to exactly ONE depth key.  16 bands of 32 rows mirror the GS page-row granularity the
// rest of the authority model already reasons in, so a CPU draw in rows 400..447 cannot invalidate
// a depth upload over rows 0..399.

static constexpr int kG591Bands = 16;
static std::atomic<uint64_t> g_g591MirBand[kG591Bands];
static std::atomic<uint64_t> g_g591MirDraws{0}, g_g591MirBumps{0};

// Called ONCE PER CPU-rasterised draw that may WRITE the private mirror.  yLo/yHi is the draw's own
// row band; over-invalidation is safe and under-invalidation is not, so the band is taken from the
// draw's own bounding box rather than from anything that narrows it later.
static inline void g591MirrorDrawWrite(int yLo, int yHi)
{
    if (!g591ZTracked())
        return;
    int lo = yLo < 0 ? 0 : yLo;
    int hi = yHi > 511 ? 511 : yHi;
    if (hi < lo)
        return;
    const int b0 = lo >> 5, b1 = hi >> 5;
    for (int b = b0; b <= b1; ++b)
        g_g591MirBand[b].fetch_add(1u, std::memory_order_relaxed);
    if (g591CensusOn())
    {
        g_g591MirDraws.fetch_add(1u, std::memory_order_relaxed);
        g_g591MirBumps.fetch_add(static_cast<uint64_t>(b1 - b0 + 1), std::memory_order_relaxed);
    }
}

struct G591PrivZ
{
    uint64_t key = 0u;
    bool valid = false;
    int lo = 0, hi = -1;                 // rows over which texture == mirror is PROVEN
    uint64_t band[kG591Bands] = {};      // the epoch at which it was proven
};
static G591PrivZ g_g591PrivZ; // flush-owner thread only, same contract as g_g178FbSnap

static bool g591BandsClean(const G591PrivZ &a, int lo, int hi)
{
    if (hi < lo)
        return false;
    const int b0 = (lo < 0 ? 0 : lo) >> 5;
    const int b1 = (hi > 511 ? 511 : hi) >> 5;
    for (int b = b0; b <= b1; ++b)
        if (g_g591MirBand[b].load(std::memory_order_relaxed) != a.band[b])
            return false;
    return true;
}

// Stamp: called immediately after a GPU->mirror depth readback has written rows [rowLo,rowHi], the
// only instant at which the resident depth texture is KNOWN to equal the mirror.  The previous
// claim survives the union iff its own bands are still clean: the batch's depth writes are confined
// to its row window by the scissor (the readback's own documented invariant), so rows outside
// [rowLo,rowHi] were not touched on either side.
static void g591PrivZStamp(uint64_t depthKey, int rowLo, int rowHi)
{
    if (!g591ZTracked() || depthKey == 0u || rowHi < rowLo)
        return;
    G591PrivZ &a = g_g591PrivZ;
    int lo = rowLo, hi = rowHi;
    if (a.valid && a.key == depthKey && g591BandsClean(a, a.lo, a.hi))
    {
        lo = std::min(lo, a.lo);
        hi = std::max(hi, a.hi);
    }
    a.key = depthKey;
    a.valid = true;
    a.lo = lo;
    a.hi = hi;
    for (int b = 0; b < kG591Bands; ++b)
        a.band[b] = g_g591MirBand[b].load(std::memory_order_relaxed);
}

static void g591PrivZDrop()
{
    if (!g591ZTracked())
        return;
    g_g591PrivZ.valid = false;
}

// ---- census + gate -----------------------------------------------------------------------------
// Blocked-conjunct ranking, taken BEFORE the gate (g513) so "the lever never fired" stays separable
// from "the lever fired and found nothing" — the distinction that cost G589 a whole phase and that
// G590 used to find privMir=100%.
enum G591ZBlk : int
{
    kG591ZBlkNotPriv = 0,
    kG591ZBlkFullInit,
    kG591ZBlkTight,
    kG591ZBlkSeeded,
    kG591ZBlkAlready,
    kG591ZBlkNoStamp,
    kG591ZBlkKey,
    kG591ZBlkRange,
    kG591ZBlkBand,
    kG591ZBlkN,
};
static const char *const kG591ZBlkName[kG591ZBlkN] = {
    "notPriv", "fullInit", "tight", "seeded", "alreadySkip",
    "noStamp", "keyMiss",  "rangeOut", "bandMoved"};
static std::atomic<uint64_t> g_g591ZN{0}, g_g591ZClean{0};
static std::atomic<uint64_t> g_g591ZBlk[kG591ZBlkN]{};
static std::atomic<uint64_t> g_g591ZSkips{0};
static std::atomic<uint64_t> g_g591ZVfyChecks{0}, g_g591ZVfyPixels{0}, g_g591ZVfyBad{0},
    g_g591ZVfyFail{0};

static void g591ZPrint(uint64_t n)
{
    char blk[256];
    int bo = 0;
    for (int i = 0; i < kG591ZBlkN; ++i)
    {
        const uint64_t v = g_g591ZBlk[i].load(std::memory_order_relaxed);
        if (v != 0u && bo < 220)
            bo += std::snprintf(blk + bo, sizeof(blk) - bo, " %s=%llu", kG591ZBlkName[i],
                                static_cast<unsigned long long>(v));
    }
    blk[bo] = '\0';
    const uint64_t clean = g_g591ZClean.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G591:zwhy] n=%llu CLEAN=%llu (%.2f%% SKIPPABLE) skips=%llu | blocked:%s | "
                 "mirror(draws=%llu bandBumps=%llu)\n",
                 static_cast<unsigned long long>(n), static_cast<unsigned long long>(clean),
                 n != 0u ? 100.0 * (double)clean / (double)n : 0.0,
                 static_cast<unsigned long long>(g_g591ZSkips.load(std::memory_order_relaxed)),
                 bo ? blk : " none",
                 static_cast<unsigned long long>(g_g591MirDraws.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g591MirBumps.load(std::memory_order_relaxed)));
}

// Returns TRUE iff the windowed depth upload is provably dead.  ALWAYS counts, so the census is
// the same call the gate makes.
static bool g591ZDecide(uint64_t depthKey, int rowLo, int rowHi, bool privateMirror, bool fullInit,
                        bool tight, bool seeded, bool alreadySkip)
{
    if (!g591ZTracked())
        return false;
    const uint64_t n = g_g591ZN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool report = g591CensusOn() && (n % 512u) == 0u;
    int blocked = -1;
    if (!privateMirror)    blocked = kG591ZBlkNotPriv;
    else if (fullInit)     blocked = kG591ZBlkFullInit;
    else if (tight)        blocked = kG591ZBlkTight;
    else if (seeded)       blocked = kG591ZBlkSeeded;
    else if (alreadySkip)  blocked = kG591ZBlkAlready;
    if (blocked < 0)
    {
        const G591PrivZ &a = g_g591PrivZ;
        if (!a.valid)                          blocked = kG591ZBlkNoStamp;
        else if (a.key != depthKey)            blocked = kG591ZBlkKey;
        else if (rowLo < a.lo || rowHi > a.hi) blocked = kG591ZBlkRange;
        else if (!g591BandsClean(a, rowLo, rowHi)) blocked = kG591ZBlkBand;
    }
    if (blocked >= 0)
    {
        g_g591ZBlk[blocked].fetch_add(1u, std::memory_order_relaxed);
        if (report)
            g591ZPrint(n);
        return false;
    }
    g_g591ZClean.fetch_add(1u, std::memory_order_relaxed);
    if (report)
        g591ZPrint(n);
    return true;
}

// Same-run oracle for slice A.  The caller built the upload anyway; prove the resident texture
// already holds exactly those values.  `checks=` beside `bad=`.
static void g591ZVerify(uint64_t depthKey, int fbW, int fbH, int glY, int rows,
                        const std::vector<float> &upload)
{
    if (!g591ZVerifyOn() || rows <= 0 || fbW <= 0 || depthKey == 0u)
        return;
    static std::vector<float> s_z; // flush-owner thread only
    if (!g242_backend_read_depth(depthKey, fbW, fbH, glY, rows, s_z) ||
        s_z.size() != static_cast<size_t>(fbW) * static_cast<size_t>(rows) ||
        upload.size() < s_z.size())
    {
        g_g591ZVfyFail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t bad = 0;
    for (size_t i = 0, n = s_z.size(); i < n; ++i)
        if (s_z[i] != upload[i])
            ++bad;
    const uint64_t checks = g_g591ZVfyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px =
        g_g591ZVfyPixels.fetch_add(s_z.size(), std::memory_order_relaxed) + s_z.size();
    const uint64_t tot = g_g591ZVfyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || checks <= 8u || (checks % 512u) == 0u)
        std::fprintf(stderr,
                     "[G591:zvfy] checks=%llu pixels=%llu bad=%llu readFail=%llu "
                     "last(rows=%d glY=%d bad=%llu)\n",
                     static_cast<unsigned long long>(checks), static_cast<unsigned long long>(px),
                     static_cast<unsigned long long>(tot),
                     static_cast<unsigned long long>(g_g591ZVfyFail.load(std::memory_order_relaxed)),
                     rows, glY, static_cast<unsigned long long>(bad));
}

// ===== SLICE B: window-scoped COLOUR authority ==================================================
//
// `snap` claims "FBO == VRAM over rows [snap.rowLo, snap.rowHi] as of snap.genSum".  The whole-
// target sum then invalidates the claim whenever ANY page of the surface moves, including pages the
// batch never touches — and the repair rewrites a different set of rows than the ones that moved.
//
// LAZY, FAIL-CLOSED STAMPING.  The per-page array is taken at the DECISION SITE itself, every time
// the whole-target test still passes.  Page generations are monotonically non-decreasing, so an
// equal SUM proves no page moved, and a stamp taken at that instant describes the snapshot's own
// instant exactly.  No snapshot-assignment site has to be enumerated: the array carries the genSum
// and coverage it was taken at, and any assignment that moves either of them trips the guard and
// the whole-target test stands.
//
// COVERAGE.  A skip leaves the FBO stale over the snapshot rows OUTSIDE the batch's window whose
// pages did move, so the claim is SHRUNK to the window that is still proven and the baseline is
// re-taken.  Without that shrink the skip would silently break the covered-row invariant a later,
// wider batch depends on.
static constexpr uint32_t kG591ColPages = 128u;
struct G591ColAuth
{
    uint32_t page = 0u, fbw = 0u;
    uint64_t genSum = 0u;
    int rowLo = 0, rowHi = -1;
    bool valid = false;
    std::array<uint64_t, kG591ColPages> gen{};
};
static std::unordered_map<uint32_t, G591ColAuth> g_g591ColAuth;

static inline uint32_t g591ColSlotLo(int rowLo, uint32_t fbw)
{
    return (static_cast<uint32_t>(std::max(0, rowLo)) / 32u) * fbw;
}
static inline uint32_t g591ColSlotHi(int rowHi, uint32_t fbw)
{
    return (static_cast<uint32_t>(std::min(511, rowHi)) / 32u) * fbw + fbw; // +1 slack slot (g178)
}

static void g591ColStamp(uint32_t key, uint32_t fbBlock, uint32_t fbw, uint64_t genSum, int snapLo,
                         int snapHi)
{
    if (!g591ColTracked() || fbw == 0u || fbw > 8u)
        return;
    G591ColAuth &a = g_g591ColAuth[key];
    a.page = fbBlock >> 5;
    a.fbw = fbw;
    a.genSum = genSum;
    a.rowLo = snapLo;
    a.rowHi = snapHi;
    a.valid = true;
    const uint32_t slots = std::min(kG591ColPages, 16u * fbw);
    for (uint32_t s = 0; s < kG591ColPages; ++s)
        a.gen[s] = (s < slots) ? g_g178PageGen[(a.page + s) & (kG178PageCount - 1u)].load(
                                     std::memory_order_relaxed)
                               : 0u;
}

static void g591ColDrop(uint32_t key)
{
    if (!g591ColTracked())
        return;
    auto it = g_g591ColAuth.find(key);
    if (it != g_g591ColAuth.end())
        it->second.valid = false;
}

enum G591CBlk : int
{
    kG591CBlkNotUpload = 0, // the existing model already skips — nothing for this lever to do
    kG591CBlkForced,        // g261ForceNoUpload / g289ConsumeOwner
    kG591CBlkSnapInv,       // no snapshot at all — non-resident repopulation (G590: residency, not a skip)
    kG591CBlkGenSame,       // the whole-target test already passes (upRowsOut)
    kG591CBlkRowsOut,       // the batch renders outside the snapshot's proven rows
    kG591CBlkNoStamp,       // no per-page array (or the fail-closed guard tripped)
    kG591CBlkMovedIn,       // a page INSIDE the batch's own window moved — genuinely live
    kG591CBlkN,
};
static const char *const kG591CBlkName[kG591CBlkN] = {"notUpload", "forced",  "snapInv", "genSame",
                                                      "rowsOut",   "noStamp", "movedIn"};
static std::atomic<uint64_t> g_g591CN{0}, g_g591CClean{0}, g_g591CSkips{0};
static std::atomic<uint64_t> g_g591CBlk[kG591CBlkN]{};
static std::atomic<uint64_t> g_g591CMovedIn{0}, g_g591CMovedOut{0}, g_g591CCleanRows{0};
static std::atomic<uint64_t> g_g591CVfyChecks{0}, g_g591CVfyPixels{0}, g_g591CVfyBad{0},
    g_g591CVfyRowBad{0}, g_g591CVfyFail{0};

static void g591CPrint(uint64_t n)
{
    char blk[256];
    int bo = 0;
    for (int i = 0; i < kG591CBlkN; ++i)
    {
        const uint64_t v = g_g591CBlk[i].load(std::memory_order_relaxed);
        if (v != 0u && bo < 220)
            bo += std::snprintf(blk + bo, sizeof(blk) - bo, " %s=%llu", kG591CBlkName[i],
                                static_cast<unsigned long long>(v));
    }
    blk[bo] = '\0';
    const uint64_t clean = g_g591CClean.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G591:cwhy] n=%llu CLEAN=%llu (%.2f%% SKIPPABLE) cleanRows=%llu skips=%llu "
                 "movedIn=%llu movedOut=%llu | blocked:%s\n",
                 static_cast<unsigned long long>(n), static_cast<unsigned long long>(clean),
                 n != 0u ? 100.0 * (double)clean / (double)n : 0.0,
                 static_cast<unsigned long long>(g_g591CCleanRows.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g591CSkips.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g591CMovedIn.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g591CMovedOut.load(std::memory_order_relaxed)),
                 bo ? blk : " none");
}

// TRUE iff the FBO is still provably equal to VRAM over exactly rows [rowLo,rowHi].
static bool g591CDecide(uint32_t key, uint32_t fbBlock, uint32_t fbw, bool uploadFb, bool forced,
                        bool snapValid, bool genSame, uint64_t snapGenSum, int snapLo, int snapHi,
                        int rowLo, int rowHi)
{
    if (!g591ColTracked())
        return false;
    const uint64_t n = g_g591CN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool report = g591CensusOn() && (n % 512u) == 0u;
    int blocked = -1;
    if (!uploadFb)      blocked = kG591CBlkNotUpload;
    else if (forced)    blocked = kG591CBlkForced;
    else if (!snapValid) blocked = kG591CBlkSnapInv;
    else if (genSame)   blocked = kG591CBlkGenSame;
    else if (rowLo < snapLo || rowHi > snapHi) blocked = kG591CBlkRowsOut;
    uint32_t movedIn = 0u, movedOut = 0u;
    if (blocked < 0)
    {
        auto it = g_g591ColAuth.find(key);
        const G591ColAuth *a =
            (it != g_g591ColAuth.end() && it->second.valid) ? &it->second : nullptr;
        // FAIL-CLOSED: the array must describe exactly the instant `snap` still claims.
        if (a == nullptr || a->genSum != snapGenSum || a->fbw != fbw ||
            a->page != (fbBlock >> 5) || a->rowLo != snapLo || a->rowHi != snapHi)
            blocked = kG591CBlkNoStamp;
        else
        {
            const uint32_t slots = std::min(kG591ColPages, 16u * fbw);
            const uint32_t inLo = g591ColSlotLo(rowLo, fbw);
            const uint32_t inHi = std::min(slots - 1u, g591ColSlotHi(rowHi, fbw));
            for (uint32_t s = 0; s < slots; ++s)
            {
                if (g_g178PageGen[(a->page + s) & (kG178PageCount - 1u)].load(
                        std::memory_order_relaxed) == a->gen[s])
                    continue;
                if (s >= inLo && s <= inHi)
                    ++movedIn;
                else
                    ++movedOut;
            }
            g_g591CMovedIn.fetch_add(movedIn, std::memory_order_relaxed);
            g_g591CMovedOut.fetch_add(movedOut, std::memory_order_relaxed);
            if (movedIn != 0u)
                blocked = kG591CBlkMovedIn;
        }
    }
    if (blocked >= 0)
    {
        g_g591CBlk[blocked].fetch_add(1u, std::memory_order_relaxed);
        if (report)
            g591CPrint(n);
        return false;
    }
    g_g591CClean.fetch_add(1u, std::memory_order_relaxed);
    g_g591CCleanRows.fetch_add(static_cast<uint64_t>(rowHi - rowLo + 1), std::memory_order_relaxed);
    if (report)
        g591CPrint(n);
    return true;
}

// Same-run oracle for slice B: the caller packed the upload anyway; read the live FBO back and
// prove it already holds those bytes.  Row-granular as well as pixel-granular, because a skip is
// row-granular (G590's rowEq law).
static void g591CVerify(uint32_t fboKey, int fbW, int fbH, int loRow, int hiRow,
                        const std::vector<uint32_t> &packed)
{
    if (!g591CVerifyOn() || hiRow < loRow || fbW <= 0)
        return;
    const int rows = hiRow - loRow + 1;
    static std::vector<uint32_t> s_px; // flush-owner thread only
    const int glY = fbH - 1 - hiRow;
    if (!g178_backend_read_color(fboKey, fbW, fbH, glY, rows, s_px) ||
        s_px.size() != static_cast<size_t>(fbW) * static_cast<size_t>(rows))
    {
        g_g591CVfyFail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t bad = 0, rowBadN = 0, tot = 0;
    for (int y = loRow; y <= hiRow; ++y)
    {
        const size_t fboRow = static_cast<size_t>(hiRow - y) * static_cast<size_t>(fbW);
        const size_t vramRow = static_cast<size_t>(fbH - 1 - y) * static_cast<size_t>(fbW);
        if (vramRow + static_cast<size_t>(fbW) > packed.size())
            break;
        uint64_t rb = 0;
        for (int x = 0; x < fbW; ++x)
        {
            ++tot;
            if (s_px[fboRow + x] != packed[vramRow + x])
                ++rb;
        }
        bad += rb;
        if (rb != 0u)
            ++rowBadN;
    }
    const uint64_t checks = g_g591CVfyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px = g_g591CVfyPixels.fetch_add(tot, std::memory_order_relaxed) + tot;
    const uint64_t totBad = g_g591CVfyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    const uint64_t totRowBad = g_g591CVfyRowBad.fetch_add(rowBadN, std::memory_order_relaxed) + rowBadN;
    if (bad != 0u || checks <= 8u || (checks % 256u) == 0u)
        std::fprintf(stderr,
                     "[G591:cvfy] checks=%llu pixels=%llu bad=%llu badRows=%llu readFail=%llu "
                     "last(rows=%d..%d bad=%llu)\n",
                     static_cast<unsigned long long>(checks), static_cast<unsigned long long>(px),
                     static_cast<unsigned long long>(totBad),
                     static_cast<unsigned long long>(totRowBad),
                     static_cast<unsigned long long>(g_g591CVfyFail.load(std::memory_order_relaxed)),
                     loRow, hiRow, static_cast<unsigned long long>(bad));
}

static void g591Report()
{
    if (!g591Tracked())
        return;
    const uint64_t zn = g_g591ZN.load(std::memory_order_relaxed);
    if (zn != 0u)
        g591ZPrint(zn);
    const uint64_t cn = g_g591CN.load(std::memory_order_relaxed);
    if (cn != 0u)
        g591CPrint(cn);
    if (g_g591ZVfyChecks.load(std::memory_order_relaxed) != 0u ||
        g_g591CVfyChecks.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G591:vfy] z(checks=%llu px=%llu bad=%llu fail=%llu) "
                     "c(checks=%llu px=%llu bad=%llu badRows=%llu fail=%llu)\n",
                     static_cast<unsigned long long>(g_g591ZVfyChecks.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591ZVfyPixels.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591ZVfyBad.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591ZVfyFail.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591CVfyChecks.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591CVfyPixels.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591CVfyBad.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591CVfyRowBad.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g591CVfyFail.load(std::memory_order_relaxed)));
}
