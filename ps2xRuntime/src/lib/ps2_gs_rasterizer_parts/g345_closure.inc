// G346 — Total-Closure slice 1 (plans/arc-total-closure.md §4): master gate + escape/row census.
//
// DC2_G345_CLOSURE=1 arms the closure behavior members for the display-grab chain (default OFF;
// master kill DC2_G345_NO_CLOSURE=1 wins). The closure arm consists of:
//   - the N-slot versioned G289 owner table (secondary slots used only under the arm),
//   - grab admission widening (secondary slot + snapshot source authority),
//   - owner-supersede prefix trim (a new full-coverage grab retires overlapped older owner blocks),
//   - post-materialize framebuffer-snapshot revalidation (FBO==VRAM after an owner publish),
//   - the G285/G343 consume-DEFER action (trim the dirty span, publish nothing, ride G261
//     residency) for grab-shaped copies that the GPU path will serve,
//   - DC2_G343_L2L_EXACT armed implicitly (g343L2lExactOn in g343_l2l_floor_census.inc).
//
// [G345:escape] census: AGGREGATE counters only (the G291 early-capped-print rule). Rides
// DC2_G345_STAT=1, or the closure arm combined with DC2_G289_STAT=1. Behavior-pure.
static bool g345ClosureOn()
{
    static const bool s_on = envFlagEnabled("DC2_G345_CLOSURE") &&
                             !envFlagEnabled("DC2_G345_NO_CLOSURE");
    return s_on;
}
static bool g345StatOn()
{
    static const bool s_stat = envFlagEnabled("DC2_G345_STAT");
    static const bool s_g289 = envFlagEnabled("DC2_G289_STAT");
    return s_stat || (g345ClosureOn() && s_g289);
}
// Strip-family fbps (MAP-0 G342/G343 census output — census-derived, per-route, not authority).
static bool g345FamilyFbp(uint32_t fbp)
{
    return fbp == 0x139u || fbp == 0x13bu || fbp == 0x143u ||
           fbp == 0x146u || fbp == 0x14au || fbp == 0x155u;
}
static std::atomic<uint64_t>
    g_g345GrabEdges{0},        // grab-shaped l2l edges (512x416 CT32 bw8 -> 0x13b/0x14a)
    g_g345GrabGpu{0},          // ... served by the G289 GPU copy
    g_g345GrabCpu{0},          // ... fell to the CPU copy (closure escape)
    g_g345RejOwnerBusy{0},     // grab reject: an owner slot already active (no slot available)
    g_g345RejPendInactive{0},  // grab reject: no active display pend and no snapshot authority
    g_g345RejPendMismatch{0},  // grab reject: pend active but tuple/shape mismatch
    g_g345RejOther{0},         // grab reject: any other admission miss
    g_g345FamUploads{0},       // family uploadFb events (the conserving WRITE side)
    g_g345FamUploadRows{0},    //   ... rows
    g_g345FamReadbacks{0},     // family FBO->VRAM publications (the READ side)
    g_g345FamReadbackRows{0},  //   ... rows
    g_g345ConsumeDefers{0},    // closure consume-defer actions (publication skipped entirely)
    g_g345ConsumeDeferRows{0}, //   ... elided rows
    g_g345Copies2nd{0},        // closure: grabs admitted into a secondary owner slot
    g_g345SnapAdmits{0},       // closure: grabs admitted via snapshot source authority
    g_g345Supersedes{0},       // closure: older-owner block ranges prefix-trimmed by a new grab
    g_g345SnapRevalidates{0},  // closure: post-materialize snapshot revalidations
    g_g345EscMat{0};           // closure: owner materializes (escapes), any cause
static std::atomic<uint64_t> g_g345EscMatCause[16] = {};
// Per-fbp split of the family rows (index order: 0x139,0x13b,0x143,0x146,0x14a,0x155).
static constexpr uint32_t kG345FamFbp[6] = {0x139u, 0x13bu, 0x143u, 0x146u, 0x14au, 0x155u};
static std::atomic<uint64_t> g_g345FamUpRowsBy[6] = {};
static std::atomic<uint64_t> g_g345FamRbRowsBy[6] = {};
// G347 census: the REAL packed upload rows (post-G290 upLoEff skip) vs the window rows above.
// Window = rowHi-rowLo+1 (what famUp counts); pack = upHi-upLoEff+1 (what actually touches VRAM).
static std::atomic<uint64_t> g_g345FamUpPackBy[6] = {};
static int g345FamIndex(uint32_t fbp)
{
    for (int i = 0; i < 6; ++i)
        if (kG345FamFbp[i] == fbp)
            return i;
    return -1;
}
static void g345NoteFamReadback(uint64_t rows)
{
    g_g345FamReadbacks.fetch_add(1u, std::memory_order_relaxed);
    g_g345FamReadbackRows.fetch_add(rows, std::memory_order_relaxed);
}
static void g345NoteFamReadbackFbp(uint32_t fbp, uint64_t rows)
{
    g345NoteFamReadback(rows);
    const int i = g345FamIndex(fbp);
    if (i >= 0)
        g_g345FamRbRowsBy[i].fetch_add(rows, std::memory_order_relaxed);
}
static void g345NoteFamUploadFbp(uint32_t fbp, uint64_t rows)
{
    g_g345FamUploads.fetch_add(1u, std::memory_order_relaxed);
    g_g345FamUploadRows.fetch_add(rows, std::memory_order_relaxed);
    const int i = g345FamIndex(fbp);
    if (i >= 0)
        g_g345FamUpRowsBy[i].fetch_add(rows, std::memory_order_relaxed);
}
static void g345NoteFamUploadPackFbp(uint32_t fbp, uint64_t rows)
{
    const int i = g345FamIndex(fbp);
    if (i >= 0)
        g_g345FamUpPackBy[i].fetch_add(rows, std::memory_order_relaxed);
}
static void g345NoteEscMat(int cause, uint32_t fbp, uint64_t rows)
{
    g_g345EscMat.fetch_add(1u, std::memory_order_relaxed);
    if (cause >= 0 && cause < 16)
        g_g345EscMatCause[cause].fetch_add(1u, std::memory_order_relaxed);
    g345NoteFamReadbackFbp(fbp, rows);
}
// G347 slice-2 premise census: split the 0x139 uploadFb population by WIDTH class (fbw=8 g290-
// seeded display batch vs fbw=8 non-seeded vs fbw=2 narrow vs other) and by CAUSE (why uploadFb
// fired: snapshot invalid / gen moved / rows outside coverage), carrying the REAL pack rows. This
// answers whether the dominant 0x139 upload is the already-redraw-skipped seeded batch or another
// population, and what re-triggers each. Behavior-pure; rides g345StatOn().
enum { kG347W8Seed = 0, kG347W8Plain = 1, kG347W2 = 2, kG347WOther = 3, kG347WidthN = 4 };
enum { kG347CSnapInv = 0, kG347CGenMoved = 1, kG347CRowsOut = 2, kG347CauseN = 3 };
static const char *const kG347WidthName[kG347WidthN] = {"w8seed", "w8plain", "w2", "wOther"};
static const char *const kG347CauseName[kG347CauseN] = {"snapInv", "genMoved", "rowsOut"};
static std::atomic<uint64_t> g_g347UpWidthEv[kG347WidthN] = {}, g_g347UpWidthPk[kG347WidthN] = {};
static std::atomic<uint64_t> g_g347UpCauseEv[kG347CauseN] = {}, g_g347UpCausePk[kG347CauseN] = {};
// Cross-width thrash test: is a 0x139 uploadFb triggered because the PREVIOUS 0x139 upload had a
// different fbw (fbp-keyed snapshot conflation → SPURIOUS, bounded-exact skippable) or the same
// fbw (genuine gen/content change → LIVE)? Split snapInv-cause events only (the closure target).
static std::atomic<uint64_t> g_g347SameW{0}, g_g347SameWPk{0}, g_g347CrossW{0}, g_g347CrossWPk{0};
static uint32_t s_g347LastUpFbw139 = 0; // flush-owner thread only
static void g347NoteUp139(int wcls, int cause, uint32_t fbw, uint64_t pk)
{
    if (wcls >= 0 && wcls < kG347WidthN)
    {
        g_g347UpWidthEv[wcls].fetch_add(1u, std::memory_order_relaxed);
        g_g347UpWidthPk[wcls].fetch_add(pk, std::memory_order_relaxed);
    }
    if (cause >= 0 && cause < kG347CauseN)
    {
        g_g347UpCauseEv[cause].fetch_add(1u, std::memory_order_relaxed);
        g_g347UpCausePk[cause].fetch_add(pk, std::memory_order_relaxed);
    }
    if (s_g347LastUpFbw139 != 0u && s_g347LastUpFbw139 != fbw)
    {
        g_g347CrossW.fetch_add(1u, std::memory_order_relaxed);
        g_g347CrossWPk.fetch_add(pk, std::memory_order_relaxed);
    }
    else if (s_g347LastUpFbw139 != 0u)
    {
        g_g347SameW.fetch_add(1u, std::memory_order_relaxed);
        g_g347SameWPk.fetch_add(pk, std::memory_order_relaxed);
    }
    s_g347LastUpFbw139 = fbw;
}
// G347 shadow oracle (DC2_G347_SHADOW=1): does the 0x139 FBO ALREADY hold the bytes this uploadFb
// is about to pack from VRAM? High eq% ⇒ the invalidation was spurious and the upload is dead
// (bounded-exact skippable). Low eq% ⇒ the FBO is genuinely stale and the upload is live. Split by
// the uploadFb cause so the crossW/snapInv (thrash) fraction is separable from genMoved.
// G348 teardown diagnosis: split the shadow read by cause × fbw-class (0=fbw8 display RTT, 1=other,
// esp. fbw2 narrow) and record read-FAIL (no FBO ⇒ non-resident) plus the g261 residency coverage
// (dirty AND the batch rows inside covLo..covHi ⇒ a snapshot-protect would have avoided the upload
// without touching the backend). Disentangles "unrelated .clear() wiped a still-resident 0x139
// snapshot" (cov=high) from "genuinely FBO-less population" (fail=high, cov=low).
static std::atomic<uint64_t> g_g347ShN[kG347CauseN][2] = {}, g_g347ShFail[kG347CauseN][2] = {},
                             g_g347ShEq[kG347CauseN][2] = {}, g_g347ShTot[kG347CauseN][2] = {},
                             g_g347ShCov[kG347CauseN][2] = {};
static void g347NoteShadow(int cause, int fbwCls, bool readOk, uint64_t eq, uint64_t tot, bool cover)
{
    if (cause < 0 || cause >= kG347CauseN || fbwCls < 0 || fbwCls > 1)
        return;
    g_g347ShN[cause][fbwCls].fetch_add(1u, std::memory_order_relaxed);
    if (!readOk)
        g_g347ShFail[cause][fbwCls].fetch_add(1u, std::memory_order_relaxed);
    else
    {
        g_g347ShEq[cause][fbwCls].fetch_add(eq, std::memory_order_relaxed);
        g_g347ShTot[cause][fbwCls].fetch_add(tot, std::memory_order_relaxed);
    }
    if (cover)
        g_g347ShCov[cause][fbwCls].fetch_add(1u, std::memory_order_relaxed);
}
static void g347PrintUp139()
{
    if (!g345StatOn())
        return;
    {
        char s[384];
        int so = 0;
        bool any = false;
        static const char *const kW[2] = {"w8", "wN"};
        for (int c = 0; c < kG347CauseN; ++c)
            for (int f = 0; f < 2; ++f)
            {
                const uint64_t n = g_g347ShN[c][f].load(std::memory_order_relaxed);
                if (n == 0u)
                    continue;
                any = true;
                const uint64_t fail = g_g347ShFail[c][f].load(std::memory_order_relaxed);
                const uint64_t eq = g_g347ShEq[c][f].load(std::memory_order_relaxed);
                const uint64_t tot = g_g347ShTot[c][f].load(std::memory_order_relaxed);
                const uint64_t cov = g_g347ShCov[c][f].load(std::memory_order_relaxed);
                so += std::snprintf(s + so, sizeof(s) - so,
                                    " %s.%s(n=%llu fail=%llu eq=%.1f%% cov=%.0f%%)",
                                    kG347CauseName[c], kW[f], (unsigned long long)n,
                                    (unsigned long long)fail,
                                    tot ? 100.0 * (double)eq / (double)tot : 0.0,
                                    n ? 100.0 * (double)cov / (double)n : 0.0);
            }
        if (any)
            std::fprintf(stderr, "[G347:shadow]%s\n", s);
    }
    char w[192];
    int wo = 0;
    for (int i = 0; i < kG347WidthN; ++i)
        wo += std::snprintf(w + wo, sizeof(w) - wo, " %s=%llu/pk%llu", kG347WidthName[i],
                            (unsigned long long)g_g347UpWidthEv[i].load(std::memory_order_relaxed),
                            (unsigned long long)g_g347UpWidthPk[i].load(std::memory_order_relaxed));
    char c[160];
    int co = 0;
    for (int i = 0; i < kG347CauseN; ++i)
        co += std::snprintf(c + co, sizeof(c) - co, " %s=%llu/pk%llu", kG347CauseName[i],
                            (unsigned long long)g_g347UpCauseEv[i].load(std::memory_order_relaxed),
                            (unsigned long long)g_g347UpCausePk[i].load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G347:up139] width%s | cause%s | thrash sameW=%llu/pk%llu crossW=%llu/pk%llu\n",
                 w, c, (unsigned long long)g_g347SameW.load(std::memory_order_relaxed),
                 (unsigned long long)g_g347SameWPk.load(std::memory_order_relaxed),
                 (unsigned long long)g_g347CrossW.load(std::memory_order_relaxed),
                 (unsigned long long)g_g347CrossWPk.load(std::memory_order_relaxed));
}
// ===== G350 slice-1: 0x139 wide (fbw=8, 512) multi-owner SIGNATURE census (behavior-pure) =====
// The color-side closure (G349) failed because the single fbp-keyed 0x139 FBO is multi-owner-stale
// (DC2_G347_SHADOW: snapInv eq=0.7% / genMoved eq=92.9% on the DECOL-persisted wide FBO). G349
// claimed "RTT wave + display strip" but g261Wave = g255Rtt && g261WaveOn() is TRUE for EVERY 0x139
// batch (0x139 = g248 target 0), so the wave flag cannot separate the owners. The multi-owner
// composition rearch (fork option 1) needs the real discriminator: for each wide 0x139 batch, bucket
// by its (uploadFb-cause × scissor-Y × row-window × draw-count) SIGNATURE. If the snapInv (stale)
// and genMoved (current) uploads carry DISTINCT signatures ⇒ two enumerable logical surfaces ⇒ the
// composition can route them to per-owner FBOs (GO → G351 spec). If they share one signature that
// merely alternates state ⇒ one surface with a teardown/alias-coherence problem (no draw-class
// discriminator; the composition is the full G242/G249/G272 persistent-ownership rework). Rides
// g345StatOn(); default path byte-identical.
enum { kG350CauseNoUp = 0, kG350CauseSnapInv = 1, kG350CauseGenMoved = 2, kG350CauseRowsOut = 3 };
static const char *const kG350CauseName[4] = {"noUp", "snapInv", "genMoved", "rowsOut"};
struct G350Sig
{
    int cause, rowLo, rowHi, scY0, scY1;
    uint32_t nDraws;
    uint64_t count;
};
static G350Sig s_g350Sigs[32]; // flush-owner thread only
static int s_g350SigN = 0;
static uint64_t s_g350Overflow = 0;
static void g350NoteWrite139(int cause, int rowLo, int rowHi, int scY0, int scY1, uint32_t nDraws)
{
    if (!g345StatOn())
        return;
    for (int i = 0; i < s_g350SigN; ++i)
    {
        G350Sig &s = s_g350Sigs[i];
        if (s.cause == cause && s.rowLo == rowLo && s.rowHi == rowHi && s.scY0 == scY0 &&
            s.scY1 == scY1 && s.nDraws == nDraws)
        {
            ++s.count;
            return;
        }
    }
    if (s_g350SigN < 32)
        s_g350Sigs[s_g350SigN++] = {cause, rowLo, rowHi, scY0, scY1, nDraws, 1u};
    else
        ++s_g350Overflow;
}
static void g350PrintOwnCensus()
{
    if (!g345StatOn() || s_g350SigN == 0)
        return;
    // Descending by count (small N; simple selection print).
    bool used[32] = {};
    std::fprintf(stderr, "[G350:owncen] distinctSigs=%d overflow=%llu (0x139 fbw=8 512-wide)\n",
                 s_g350SigN, (unsigned long long)s_g350Overflow);
    for (int rank = 0; rank < s_g350SigN && rank < 12; ++rank)
    {
        int best = -1;
        uint64_t bestC = 0;
        for (int i = 0; i < s_g350SigN; ++i)
            if (!used[i] && s_g350Sigs[i].count >= bestC)
            {
                bestC = s_g350Sigs[i].count;
                best = i;
            }
        if (best < 0)
            break;
        used[best] = true;
        const G350Sig &s = s_g350Sigs[best];
        std::fprintf(stderr, "[G350:sig] cause=%s rows=%d..%d scY=%d..%d nDraws=%u count=%llu\n",
                     kG350CauseName[s.cause < 0 || s.cause > 3 ? 0 : s.cause], s.rowLo, s.rowHi,
                     s.scY0, s.scY1, s.nDraws, (unsigned long long)s.count);
    }
}

// ===== G351 slice-2 step-1: 0x139 consumer→surface census (behavior-pure) =====
// G350 enumerated the two wide 0x139 owners (A = scene rows 0..415, many draws; B = full-screen
// copy sprite rows 0..447, single draw). Before the surface-key FBO split can route Owner B to a
// sibling FBO, every consumer edge of 0x139 must be attributed to the surface whose content it
// reads (the G272 lost-content risk: Owner B on a sibling FBO changes what the fbp-keyed FBO and
// its VRAM publication hold). Mechanism: a last-wide-writer latch updated at the flush classify
// site, noted at each consumer edge — g261Materialize (cause-coded FBO→VRAM publication, the
// VRAM-medium consumers' demand edge), the G280/G282 fan-out FBO→FBO copies (FBO-medium), and the
// pre-draw uploadFb edge (whose FBO content is being overwritten). Rides g345StatOn(); default
// path byte-identical. Flush-owner thread only (same discipline as s_g350Sigs).
enum { kG351WNone = 0, kG351WScene = 1, kG351WSprite = 2, kG351WOther = 3, kG351WriterN = 4 };
static const char *const kG351WriterName[kG351WriterN] = {"none", "A", "B", "oth"};
enum { kG351SiteMat = 0, kG351SiteFan = 1, kG351SiteOvl = 2, kG351SiteUp = 3, kG351SiteN = 4 };
static const char *const kG351SiteName[kG351SiteN] = {"mat", "fan", "ovl", "up"};
static const char *const kG351MatCauseName[8] = {"cpuRep", "inline", "upload", "local",
                                                 "l2h",    "texAli", "resync", "fail"};
static int s_g351LastW139 = kG351WNone; // last wide (fbw=8/512) 0x139 writer surface
static uint64_t s_g351Writes[kG351WriterN] = {};
static uint64_t s_g351Con[kG351SiteN][8][kG351WriterN] = {}; // [site][cause|0][writer] events
static uint64_t s_g351ConRows[kG351SiteN][kG351WriterN] = {};
// ===== G351 slice-2 step-2: the surface-key FBO split (default-off behavior arm) =====
// [G351:con] proved both wide owners are consumed via VRAM only (mat local/resync while latch=A,
// mat texAli 448 rows while latch=B; fan/ovl = 0) — so the split keeps VRAM as the coherence
// medium (DECOL pattern) and only re-routes WHICH GL FBO each surface draws into and which FBO
// each 0x139 materialize reads. Owner B (the full-screen copy sprite, rows 0..447) gets a private
// sibling FBO key; Owner A's scene FBO becomes uncontended. Requires the G349 DECOL substrate
// (the narrow width population must already be de-collided or the width collision would keep
// destroying both surfaces' FBOs). Master kill DC2_G351_NO_SPLIT=1.
static constexpr uint32_t kG351Sprite139 = 0xfffffff4u;
static bool g351SplitOn()
{
    static const bool s_on = envFlagEnabled("DC2_G351_SPLIT") &&
                             !envFlagEnabled("DC2_G351_NO_SPLIT") &&
                             envFlagEnabled("DC2_G349_DECOL") &&
                             !envFlagEnabled("DC2_G349_NO_DECOL");
    return s_on;
}
// Which FBO key holds the CURRENT wide-0x139 content, tracked per ROW BAND. Only the exact
// single-draw copy sprite is routed to the sibling (merged frame-end batches that contain the
// scene AND the sprite stay on the fbp key — routing them would strand scene draws). Band 0 =
// rows 0..415 (scene + sprite), band 1 = rows 416..447 (sprite-only — the scene never writes
// there, and 32-row page tiles never straddle 415/416 since 416 = 13*32).
static bool s_g351Band0IsB = false, s_g351Band1IsB = false; // flush-owner thread only
static void g351NoteWide139Routed(bool routedB, int rowLo, int rowHi)
{
    if (rowLo <= 415)
        s_g351Band0IsB = routedB;
    if (rowHi >= 416)
        s_g351Band1IsB = routedB;
}
static uint32_t g351Band0Key139() { return s_g351Band0IsB ? kG351Sprite139 : 0x139u; }
static uint32_t g351Band1Key139() { return s_g351Band1IsB ? kG351Sprite139 : 0x139u; }
// Loud counters: mixed-band materialize (two-part read), and a GPU-GPU copy that sources the wide
// 0x139 FBO while the sprite band is routed away (the G272 risk this split must NOT trip — zero on
// MAP-0 where fan/ovl consumers are absent; a non-zero value on any route BLOCKS promotion there).
static uint64_t s_g351MixedMat = 0, s_g351FanCross = 0;
// Fail-closed detector at a GPU-GPU copy sourcing wide 0x139: if the sprite band has been routed
// to the sibling FBO, the 0x139 FBO's rows 416..447 no longer hold current content. Returns true
// when the copy is safe under the current routing (band1 still on the fbp key, or split off).
static bool g351FanSourceSafe()
{
    if (!g351SplitOn())
        return true;
    if (s_g351Band1IsB)
    {
        ++s_g351FanCross;
        return false;
    }
    return true;
}
// Classify a wide 0x139 batch write window into the G350 owner signature (row-extent primary,
// scissor corroborating; nDraws deliberately NOT load-bearing — G350 Stale-When).
static int g351ClassifyWide139(int rowHi, int scY1)
{
    if (rowHi <= 415)
        return kG351WScene;
    if (rowHi >= 416 && scY1 >= 440)
        return kG351WSprite;
    return kG351WOther;
}
static void g351NoteWide139Write(int cls)
{
    if (cls < 0 || cls >= kG351WriterN)
        return;
    if (g345StatOn())
        ++s_g351Writes[cls];
    s_g351LastW139 = cls; // behavior latch: also feeds g351MatKey139() when the split is armed
}
static void g351NoteCon(int site, int sub, uint64_t rows)
{
    if (!g345StatOn() || site < 0 || site >= kG351SiteN)
        return;
    const int w = s_g351LastW139;
    ++s_g351Con[site][sub & 7][w];
    s_g351ConRows[site][w] += rows;
}
static void g351PrintCon()
{
    if (!g345StatOn())
        return;
    std::fprintf(stderr,
                 "[G351:writes] split=%d A=%llu B=%llu oth=%llu mixedMat=%llu fanCross=%llu\n",
                 g351SplitOn() ? 1 : 0,
                 (unsigned long long)s_g351Writes[kG351WScene],
                 (unsigned long long)s_g351Writes[kG351WSprite],
                 (unsigned long long)s_g351Writes[kG351WOther],
                 (unsigned long long)s_g351MixedMat,
                 (unsigned long long)s_g351FanCross);
    for (int site = 0; site < kG351SiteN; ++site)
    {
        char s[512];
        int so = 0;
        bool any = false;
        for (int c = 0; c < 8; ++c)
            for (int w = 0; w < kG351WriterN; ++w)
            {
                const uint64_t n = s_g351Con[site][c][w];
                if (n == 0u || so >= (int)sizeof(s) - 48)
                    continue;
                any = true;
                const char *sub = "cnt";
                if (site == kG351SiteMat)
                    sub = kG351MatCauseName[c];
                else if (site == kG351SiteUp)
                    sub = kG350CauseName[c < 4 ? c : 0];
                so += std::snprintf(s + so, sizeof(s) - so, " %s.%s=%llu", sub,
                                    kG351WriterName[w], (unsigned long long)n);
            }
        if (!any)
            continue;
        char r[128];
        int ro = 0;
        for (int w = 0; w < kG351WriterN; ++w)
            if (s_g351ConRows[site][w] != 0u)
                ro += std::snprintf(r + ro, sizeof(r) - ro, " %s:%lluR", kG351WriterName[w],
                                    (unsigned long long)s_g351ConRows[site][w]);
        std::fprintf(stderr, "[G351:con] site=%s%s rows%s\n", kG351SiteName[site], s, r);
    }
}

// Printed from the same per-frame stat cadence as [G289:stat] (flush-owner thread).
static void g345PrintEscape()
{
    if (!g345StatOn())
        return;
    char causes[160];
    int off = 0;
    causes[0] = '\0';
    for (int c = 0; c < 16; ++c)
    {
        const uint64_t v = g_g345EscMatCause[c].load(std::memory_order_relaxed);
        if (v != 0u && off < static_cast<int>(sizeof(causes)) - 24)
            off += std::snprintf(causes + off, sizeof(causes) - off, " c%d=%llu", c,
                                 static_cast<unsigned long long>(v));
    }
    char byFbp[224];
    int boff = 0;
    byFbp[0] = '\0';
    for (int i = 0; i < 6; ++i)
    {
        const uint64_t up = g_g345FamUpRowsBy[i].load(std::memory_order_relaxed);
        const uint64_t rb = g_g345FamRbRowsBy[i].load(std::memory_order_relaxed);
        const uint64_t pk = g_g345FamUpPackBy[i].load(std::memory_order_relaxed);
        if ((up | rb | pk) != 0u && boff < static_cast<int>(sizeof(byFbp)) - 56)
            boff += std::snprintf(byFbp + boff, sizeof(byFbp) - boff,
                                  " %x:%llu/%llu/pk%llu", kG345FamFbp[i],
                                  (unsigned long long)up, (unsigned long long)rb,
                                  (unsigned long long)pk);
    }
    std::fprintf(stderr, "[G345:byfbp] up/rb rows%s\n", byFbp);
    std::fprintf(stderr,
                 "[G345:escape] closure=%d grabs=%llu gpu=%llu cpu=%llu "
                 "rej(owner=%llu pendOff=%llu pendMis=%llu other=%llu) "
                 "famUp=%llu/%lluR famRb=%llu/%lluR defer=%llu/%lluR "
                 "slot2=%llu snapAdmit=%llu supersede=%llu snapReval=%llu escMat=%llu%s\n",
                 g345ClosureOn() ? 1 : 0,
                 (unsigned long long)g_g345GrabEdges.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345GrabGpu.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345GrabCpu.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345RejOwnerBusy.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345RejPendInactive.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345RejPendMismatch.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345RejOther.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345FamUploads.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345FamUploadRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345FamReadbacks.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345FamReadbackRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345ConsumeDefers.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345ConsumeDeferRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345Copies2nd.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345SnapAdmits.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345Supersedes.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345SnapRevalidates.load(std::memory_order_relaxed),
                 (unsigned long long)g_g345EscMat.load(std::memory_order_relaxed),
                 causes);
}
