// ===== G590: SURFACE AUTHORITY — PER-TARGET CENSUS + GENERALIZED SHADOW ORACLE =====
//
// WHY THIS FILE EXISTS. G589 closed the command-graph half of the GPU-VU redesign (coalescing
// `mergeable=0.00%`, backend overlap 5-7 us of handoff, the drain memo `hits=0`) and handed over
// ONE surviving direction: surface authority. It priced that direction as "~40% of GPU backend
// work is surface upload, ~17% readback, only ~18% drawing" and labelled it THE POLE.
//
// ⛔ THAT LABEL WAS A SHARE, NOT A PRICE — the project's own standing law (a PC-sample share is
// not a price; a call-class argument is not a price). De-cumulating `[G415:stage]` over the same
// window it was measured on (`s05 n=1021..2461`, 1440 frames, 77,400 backend jobs) converts it:
//
//     backend TOTAL          3.2650 ms/frame      of a 36.20 ms/f GS-own window   =  9.0%
//       up  colour upload    0.9466 ms/frame  29.0% of backend                    =  2.6%
//       z   depth upload     0.5622 ms/frame  17.2% of backend                    =  1.6%
//       rb  readback         0.3577 ms/frame  11.0% of backend                    =  1.0%
//       tex texture upload   0.4390 ms/frame  13.4% of backend                    =  1.2%
//       draw actual drawing  0.6531 ms/frame  20.0% of backend                    =  1.8%
//
// Add the FRONT-END half of the same family — `[G298:profile] fbz=0.055 ms/flush` (the colour+Z
// pack out of guest VRAM) x 56.8 flushes/frame = ~3.1 ms/frame — and the whole surface-transfer
// family is ~5 ms/frame of a 36.2 ms/frame window: **~14%, and the largest architecturally
// coherent family, but NOT the pole.** Every number this phase produces is quoted against that.
//
// WHAT IS ACTUALLY BEING ASKED. The goal is an authority model: VU output, render targets, depth
// surfaces and dependent state should stay GPU-resident across commands whenever correctness
// permits, so a redesigned GPU-VU executor can feed XGKICK/GS work in without mandatory CPU
// materialization. That is not a millisecond question first — it is an EVENT question:
//
//     how often is GPU authority LOST, per target, and WHY?
//
// THE TWO INSTRUMENTS.
//
//   DC2_G590_CENSUS=1  -> [G590:auth]  Behaviour-pure. Every colour-upload decision, every depth-
//                         upload decision, every FBO->VRAM materialization and every colour
//                         readback, bucketed by TARGET x REASON, counted in both EVENTS and ROWS.
//                         Rows matter: `0x13a` uploads 512 rows to issue 2 draws while
//                         `0x139/skip+z` uploads 73 rows to issue 71, so an event ranking inverts
//                         the order (the standing "rank by entries, not events" law).
//                         ⭐ It tracks ANY fbp, not just the 14 residency slots. `0x0`/`0x68` have
//                         no `g248TargetIndex` slot at all and are invisible to every existing
//                         per-target census — yet `[G415]` bills them 0.51 ms/frame of upload+Z.
//
//   DC2_G590_SHADOW=1  -> [G590:shadow]  The roadmap's stated ENTRY CONDITION for this whole
//                         direction, generalized. G347's shadow oracle asked "does the FBO already
//                         hold the bytes this upload is about to write?" but was hardcoded to
//                         `fbp == 0x139` and to colour. This asks it for EVERY target and for
//                         DEPTH as well, split by the same reason code as the census.
//
//                             high eq%  => the upload is DEAD. The GPU copy was already correct;
//                                          authority was surrendered for nothing. A skip lever has
//                                          something to skip TO.
//                             low  eq%  => the upload is LIVE. The FBO genuinely is stale, so the
//                                          answer is RESIDENCY (move the writer), not a skip — and
//                                          G304's multi-owner physical-alias local minimum applies,
//                                          which has already refuted six bounded skip levers
//                                          (G338/G340/G342/G343/G349/G351).
//
// ⚠️ THE SHADOW ORACLE IS PERTURBING. It issues a blocking `glReadPixels` per upload. It is a
// DIAGNOSTIC ONLY: never arm it inside a timing arm, and never quote a frame time from a run that
// had it on. The census (`DC2_G590_CENSUS`) is behaviour-pure and safe to leave on for counting,
// though its atomics are not free either — do not quote payoff from a census run.
//
// ⚠️ BOTH PRINT THEIR COMPARISON COUNT. G589's slice-1 oracle printed `badResult=0 badKey=0` with
// `checks=0` — a failed run wearing a passing run's clothes. Every line below carries `n=` and
// `px=` beside its `eq=`, and `px=0` means the oracle compared NOTHING.
//
// All counters are whole-run cumulative and print on a fixed event cadence, so a window figure
// needs bracketing against the nearest preceding `[G154:perf] n=` marker at each end
// (`tools/g590_window.py`).

static bool g590CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G590_CENSUS");
    return on;
}

static bool g590ShadowOn()
{
    static const bool on = envFlagEnabled("DC2_G590_SHADOW");
    return on;
}

static bool g590Armed()
{
    static const bool on = g590CensusOn() || g590ShadowOn();
    return on;
}

// ---- slot map: ANY fbp, not just the 14 residency targets --------------------------------------
// FRAME.FBP is 9 bits, so a 512-entry direct map is the complete key space (the G527 pattern).
// 0 = unassigned, else slot+1. Assignment is a CAS so the GS worker and the EE frame barrier
// cannot both claim the same slot.
static constexpr int kG590Slots = 24;
static std::atomic<uint8_t> g_g590SlotOf[512];
static std::atomic<uint32_t> g_g590SlotN{0};
static uint32_t g_g590SlotFbp[kG590Slots] = {};
static std::atomic<uint32_t> g_g590SlotOverflow{0};

static int g590Slot(uint32_t fbp)
{
    const uint32_t key = fbp & 511u;
    const uint8_t have = g_g590SlotOf[key].load(std::memory_order_relaxed);
    if (have != 0u)
        return static_cast<int>(have) - 1;
    const uint32_t next = g_g590SlotN.fetch_add(1u, std::memory_order_relaxed);
    if (next >= static_cast<uint32_t>(kG590Slots))
    {
        g_g590SlotOverflow.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    uint8_t expected = 0u;
    const uint8_t want = static_cast<uint8_t>(next + 1u);
    if (g_g590SlotOf[key].compare_exchange_strong(expected, want, std::memory_order_relaxed))
    {
        g_g590SlotFbp[next] = fbp;
        return static_cast<int>(next);
    }
    return static_cast<int>(expected) - 1; // lost the race; the winner's slot is correct
}

// ---- reason codes ------------------------------------------------------------------------------
enum G590ColCause : int
{
    kG590ColSkipRes = 0,    // g261ForceNoUpload — residency covers these rows; authority HELD
    kG590ColSkipSnap = 1,   // snapshot equality — FBO provably still equals VRAM
    kG590ColSkipOwner = 2,  // g289ConsumeOwner — an owner will supply the pixels
    kG590ColUpSnapInv = 3,  // UPLOAD: no snapshot (a CPU fallback / clear dropped it)
    kG590ColUpGenMoved = 4, // UPLOAD: the fb pages' write-gen moved — a real guest/CPU writer
    kG590ColUpRowsOut = 5,  // UPLOAD: snapshot current but this batch renders outside its window
    kG590ColN = 6,
};
static const char *const kG590ColName[kG590ColN] = {"skipRes",   "skipSnap",   "skipOwner",
                                                    "upSnapInv", "upGenMoved", "upRowsOut"};

enum G590ZCause : int
{
    kG590ZSkipPend = 0, // G278 pending owner — the backend texture is the sole current copy
    kG590ZSkipRes = 1,  // G274 resident depth + VRAM Z provably unchanged
    kG590ZUpFull = 2,   // UPLOAD: full init
    kG590ZUpWin = 3,    // UPLOAD: windowed
    kG590ZUpSeed = 4,   // UPLOAD: G290 partial seed (rows 416..447)
    kG590ZN = 5,
};
static const char *const kG590ZName[kG590ZN] = {"zSkipPend", "zSkipRes", "zUpFull", "zUpWin",
                                                "zUpSeed"};

static const char *const kG590MatName[8] = {"cpuReplay", "inline", "upload", "local",
                                            "l2h",       "texAlias", "resync", "fail"};

// ---- ledgers -----------------------------------------------------------------------------------
static std::atomic<uint64_t> g_g590ColN[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ColRows[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ZN[kG590Slots][kG590ZN]{};
static std::atomic<uint64_t> g_g590ZRows[kG590Slots][kG590ZN]{};
static std::atomic<uint64_t> g_g590MatN[kG590Slots][8]{};
static std::atomic<uint64_t> g_g590MatRows[kG590Slots][8]{};
static std::atomic<uint64_t> g_g590RbN[kG590Slots]{};
static std::atomic<uint64_t> g_g590RbRows[kG590Slots]{};
static std::atomic<uint8_t> g_g590Resident[kG590Slots]{}; // 1 if g248TargetIndex(fbp) >= 0

// shadow oracle: colour and depth, both split by the same cause code as the census
static std::atomic<uint64_t> g_g590ShN[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ShFail[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ShEq[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ShTot[kG590Slots][kG590ColN]{};
// ROW-exact equality: a row is "eq" only if every pixel in it matched. A skip lever operates on
// ROWS, so a 99% pixel match that misses one pixel per row buys nothing — quote both.
static std::atomic<uint64_t> g_g590ShRowEq[kG590Slots][kG590ColN]{};
static std::atomic<uint64_t> g_g590ShRowTot[kG590Slots][kG590ColN]{};

static std::atomic<uint64_t> g_g590ZShN[kG590Slots][kG590ZN]{};
static std::atomic<uint64_t> g_g590ZShFail[kG590Slots][kG590ZN]{};
static std::atomic<uint64_t> g_g590ZShEq[kG590Slots][kG590ZN]{};
static std::atomic<uint64_t> g_g590ZShTot[kG590Slots][kG590ZN]{};

static std::atomic<uint64_t> g_g590Decisions{0};

// ---- printing ----------------------------------------------------------------------------------
static void g590Print()
{
    const uint32_t nSlots =
        std::min<uint32_t>(g_g590SlotN.load(std::memory_order_relaxed), kG590Slots);
    std::fprintf(stderr, "[G590:auth] decisions=%llu slots=%u overflow=%u",
                 static_cast<unsigned long long>(g_g590Decisions.load(std::memory_order_relaxed)),
                 nSlots, g_g590SlotOverflow.load(std::memory_order_relaxed));
    for (uint32_t s = 0; s < nSlots; ++s)
    {
        uint64_t any = 0;
        for (int c = 0; c < kG590ColN; ++c)
            any += g_g590ColN[s][c].load(std::memory_order_relaxed);
        for (int c = 0; c < kG590ZN; ++c)
            any += g_g590ZN[s][c].load(std::memory_order_relaxed);
        if (any == 0u)
            continue;
        std::fprintf(stderr, " | 0x%x%s", g_g590SlotFbp[s],
                     g_g590Resident[s].load(std::memory_order_relaxed) ? "R" : "-");
        for (int c = 0; c < kG590ColN; ++c)
        {
            const uint64_t n = g_g590ColN[s][c].load(std::memory_order_relaxed);
            if (n == 0u)
                continue;
            std::fprintf(stderr, " %s=%llu/r%llu", kG590ColName[c],
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(
                             g_g590ColRows[s][c].load(std::memory_order_relaxed)));
        }
        for (int c = 0; c < kG590ZN; ++c)
        {
            const uint64_t n = g_g590ZN[s][c].load(std::memory_order_relaxed);
            if (n == 0u)
                continue;
            std::fprintf(stderr, " %s=%llu/r%llu", kG590ZName[c],
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(
                             g_g590ZRows[s][c].load(std::memory_order_relaxed)));
        }
        for (int c = 0; c < 8; ++c)
        {
            const uint64_t n = g_g590MatN[s][c].load(std::memory_order_relaxed);
            if (n == 0u)
                continue;
            std::fprintf(stderr, " mat.%s=%llu/r%llu", kG590MatName[c],
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(
                             g_g590MatRows[s][c].load(std::memory_order_relaxed)));
        }
        const uint64_t rb = g_g590RbN[s].load(std::memory_order_relaxed);
        if (rb != 0u)
            std::fprintf(stderr, " rb=%llu/r%llu", static_cast<unsigned long long>(rb),
                         static_cast<unsigned long long>(
                             g_g590RbRows[s].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "\n");

    if (!g590ShadowOn())
        return;
    std::fprintf(stderr, "[G590:shadow]");
    for (uint32_t s = 0; s < nSlots; ++s)
    {
        for (int c = 0; c < kG590ColN; ++c)
        {
            const uint64_t n = g_g590ShN[s][c].load(std::memory_order_relaxed);
            if (n == 0u)
                continue;
            const uint64_t tot = g_g590ShTot[s][c].load(std::memory_order_relaxed);
            const uint64_t rowTot = g_g590ShRowTot[s][c].load(std::memory_order_relaxed);
            std::fprintf(
                stderr, " 0x%x.%s(n=%llu fail=%llu px=%llu eq=%.2f%% rowEq=%.2f%%)",
                g_g590SlotFbp[s], kG590ColName[c], static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(g_g590ShFail[s][c].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(tot),
                tot ? 100.0 * static_cast<double>(g_g590ShEq[s][c].load(std::memory_order_relaxed)) /
                          static_cast<double>(tot)
                    : 0.0,
                rowTot ? 100.0 *
                             static_cast<double>(
                                 g_g590ShRowEq[s][c].load(std::memory_order_relaxed)) /
                             static_cast<double>(rowTot)
                       : 0.0);
        }
        for (int c = 0; c < kG590ZN; ++c)
        {
            const uint64_t n = g_g590ZShN[s][c].load(std::memory_order_relaxed);
            if (n == 0u)
                continue;
            const uint64_t tot = g_g590ZShTot[s][c].load(std::memory_order_relaxed);
            std::fprintf(
                stderr, " 0x%x.%s(n=%llu fail=%llu px=%llu eq=%.2f%%)", g_g590SlotFbp[s],
                kG590ZName[c], static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(
                    g_g590ZShFail[s][c].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(tot),
                tot ? 100.0 *
                          static_cast<double>(g_g590ZShEq[s][c].load(std::memory_order_relaxed)) /
                          static_cast<double>(tot)
                    : 0.0);
        }
    }
    std::fprintf(stderr, "\n");
}

// ---- census entry points -----------------------------------------------------------------------

// The colour-upload decision, called immediately after `s_batch.uploadFb` is computed. Returns the
// cause code so the shadow oracle at the pack site can reuse it without recomputing (the pack site
// runs AFTER g277 may have mutated `snap`, which is exactly the bug G347 documented).
static int g590ClassifyColor(bool uploadFb, bool forceNoUpload, bool consumeOwner, bool snapValid,
                             bool genSame)
{
    if (!uploadFb)
        return forceNoUpload ? kG590ColSkipRes
                             : (consumeOwner ? kG590ColSkipOwner : kG590ColSkipSnap);
    if (!snapValid)
        return kG590ColUpSnapInv;
    return genSame ? kG590ColUpRowsOut : kG590ColUpGenMoved;
}

static void g590NoteColor(uint32_t fbp, int cause, int rows)
{
    if (!g590Armed() || cause < 0 || cause >= kG590ColN)
        return;
    const int s = g590Slot(fbp);
    if (s < 0)
        return;
    if (g248TargetIndex(fbp) >= 0)
        g_g590Resident[s].store(1u, std::memory_order_relaxed);
    g_g590ColN[s][cause].fetch_add(1u, std::memory_order_relaxed);
    g_g590ColRows[s][cause].fetch_add(rows > 0 ? static_cast<uint64_t>(rows) : 0u,
                                      std::memory_order_relaxed);
    if ((g_g590Decisions.fetch_add(1u, std::memory_order_relaxed) + 1u) % 4096u == 0u)
        g590Print();
}

static void g590NoteDepth(uint32_t fbp, int cause, int rows)
{
    if (!g590Armed() || cause < 0 || cause >= kG590ZN)
        return;
    const int s = g590Slot(fbp);
    if (s < 0)
        return;
    g_g590ZN[s][cause].fetch_add(1u, std::memory_order_relaxed);
    g_g590ZRows[s][cause].fetch_add(rows > 0 ? static_cast<uint64_t>(rows) : 0u,
                                    std::memory_order_relaxed);
}

static void g590NoteReadback(uint32_t fbp, int rows)
{
    if (!g590Armed())
        return;
    const int s = g590Slot(fbp);
    if (s < 0)
        return;
    g_g590RbN[s].fetch_add(1u, std::memory_order_relaxed);
    g_g590RbRows[s].fetch_add(rows > 0 ? static_cast<uint64_t>(rows) : 0u,
                              std::memory_order_relaxed);
}

// FBO -> guest VRAM publication: the moment GPU authority is surrendered back to the CPU. This is
// the edge a GPU-resident model has to delete, so it is counted by TARGET x CAUSE x ROWS rather
// than by the two separate existing counters (`g_g261MatByTarget` per target, `g_g310MatN` per
// cause) which cannot be crossed after the fact.
static void g590NoteMaterialize(uint32_t fbp, int cause, int rows)
{
    if (!g590Armed())
        return;
    const int s = g590Slot(fbp);
    if (s < 0)
        return;
    g_g590MatN[s][cause & 7].fetch_add(1u, std::memory_order_relaxed);
    g_g590MatRows[s][cause & 7].fetch_add(rows > 0 ? static_cast<uint64_t>(rows) : 0u,
                                          std::memory_order_relaxed);
}

// ---- the shadow oracle -------------------------------------------------------------------------
//
// Called at the colour pack site, after `s_batch.fbPixels` holds the packed VRAM bytes for rows
// `loRow..hiRow` and BEFORE the backend uploads them. Reads the same rows back out of the live FBO
// and compares. `fboKey` is the surface key the backend will bind (G351 routes an Owner-B sprite to
// a sibling FBO), which is not always `fbp`.
//
// ⚠️ PERTURBING: one blocking readback per upload. Diagnostic only.
static void g590ShadowColor(uint32_t fbp, uint32_t fboKey, int fbW, int fbH, int loRow, int hiRow,
                            int cause, const std::vector<uint32_t> &packed)
{
    if (!g590ShadowOn() || cause < kG590ColUpSnapInv || cause >= kG590ColN)
        return;
    const int s = g590Slot(fbp);
    if (s < 0 || hiRow < loRow || fbW <= 0)
        return;
    const int rows = hiRow - loRow + 1;
    static std::vector<uint32_t> s_px; // flush-owner thread only
    const int glY = fbH - 1 - hiRow;
    const bool readOk =
        g178_backend_read_color(fboKey != 0u ? fboKey : fbp, fbW, fbH, glY, rows, s_px) &&
        s_px.size() == static_cast<size_t>(fbW) * static_cast<size_t>(rows);
    g_g590ShN[s][cause].fetch_add(1u, std::memory_order_relaxed);
    if (!readOk)
    {
        g_g590ShFail[s][cause].fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t eq = 0, tot = 0, rowEq = 0, rowTot = 0;
    for (int y = loRow; y <= hiRow; ++y)
    {
        const size_t fboRow = static_cast<size_t>(hiRow - y) * static_cast<size_t>(fbW);
        const size_t vramRow = static_cast<size_t>(fbH - 1 - y) * static_cast<size_t>(fbW);
        if (vramRow + static_cast<size_t>(fbW) > packed.size())
            break;
        uint64_t rowBad = 0;
        for (int x = 0; x < fbW; ++x)
        {
            ++tot;
            if (s_px[fboRow + x] == packed[vramRow + x])
                ++eq;
            else
                ++rowBad;
        }
        ++rowTot;
        if (rowBad == 0u)
            ++rowEq;
    }
    g_g590ShEq[s][cause].fetch_add(eq, std::memory_order_relaxed);
    g_g590ShTot[s][cause].fetch_add(tot, std::memory_order_relaxed);
    g_g590ShRowEq[s][cause].fetch_add(rowEq, std::memory_order_relaxed);
    g_g590ShRowTot[s][cause].fetch_add(rowTot, std::memory_order_relaxed);
}

// Depth side. `upload` holds the float depth rows the front end is about to push at GL row `glY`.
static void g590ShadowDepth(uint32_t fbp, uint64_t depthKey, int fbW, int fbH, int glY, int rows,
                            int cause, const std::vector<float> &upload)
{
    if (!g590ShadowOn() || cause < kG590ZUpFull || cause >= kG590ZN)
        return;
    const int s = g590Slot(fbp);
    if (s < 0 || rows <= 0 || fbW <= 0 || depthKey == 0u)
        return;
    static std::vector<float> s_z; // flush-owner thread only
    const bool readOk = g242_backend_read_depth(depthKey, fbW, fbH, glY, rows, s_z) &&
                        s_z.size() == static_cast<size_t>(fbW) * static_cast<size_t>(rows) &&
                        upload.size() >= s_z.size();
    g_g590ZShN[s][cause].fetch_add(1u, std::memory_order_relaxed);
    if (!readOk)
    {
        g_g590ZShFail[s][cause].fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t eq = 0;
    const size_t n = s_z.size();
    for (size_t i = 0; i < n; ++i)
        if (s_z[i] == upload[i])
            ++eq;
    g_g590ZShEq[s][cause].fetch_add(eq, std::memory_order_relaxed);
    g_g590ZShTot[s][cause].fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

// ===== G590 slice 1: WINDOW-SCOPED DEPTH AUTHORITY =====================================
//
// WHAT THE ORACLE FOUND. `DC2_G590_SHADOW=1` on `s05 n=1021..2461`, four targets, every windowed
// depth upload compared against the live GL depth texture BEFORE it is pushed:
//
//     0x139.zUpWin  n=2327  fail=0  px=478,330,880  eq=100.00%
//     0x0.zUpWin    n=730   fail=0  px=155,484,160  eq=100.00%
//     0x68.zUpWin   n=729   fail=0  px=155,271,168  eq=100.00%
//     0x159.zUpWin  n=7     fail=0  px=  1,490,944  eq=100.00%
//
// 2.82 BILLION depth values on the whole run, ZERO differences. Every windowed depth upload writes
// values the resident texture already holds. (`px=` is printed beside `eq=` deliberately: an
// oracle that compared nothing prints a perfect score — G589's slice-1 failure mode.)
//
// WHY IT FIRES ANYWAY — THE DEFECT. G274's residency test is
//
//     unchanged = (g_g274LastZGen[depthKey] == g178GenSumRect(zbp, fbw, Z24, 0,0, fbW, **512**))
//
// a WHOLE-TARGET, 512-row generation sum, while the upload it gates writes only rows
// `depthRowLo..depthRowHi`. So a write to any page of the surface — including page rows this batch
// never touches — invalidates the whole depth surface, and the "repair" then rewrites a DIFFERENT
// set of rows than the ones that were invalidated. **The upload can never fix what invalidated
// it.** That is exactly why the oracle reads 100.00%: the rows being uploaded are, by construction,
// rows nothing wrote.
//
// THE LEVER. Narrow the equality test to the rows the upload actually covers. Per depth key, snap
// the per-page generations at the same instants G274 stamps its whole-target baseline (both of
// them, and drop the snapshot wherever G274 erases its stamp). At the decision, ask whether any
// page overlapping THIS batch's row window has moved since. If none has, the resident texture is
// authoritative for those rows and the upload is dead.
//
// EXACTNESS ARGUMENT (and why it is not the same claim as "nothing changed"):
//   * the baseline is stamped immediately after a full-target GPU->VRAM depth writeback, so at that
//     instant the resident texture equals guest VRAM over the whole surface;
//   * a page whose generation is unchanged since then has had no writer;
//   * therefore the resident texture still equals VRAM over the batch's window;
//   * the lever only ever ADDS a skip on top of G274's, and only when G274's own baseline exists
//     (`prevKnown`), so every erase path that disarms G274 disarms this too;
//   * the page walk carries `g178BumpRectImpl`'s +1 slack page, so it can only ever see MORE
//     bumps than the bump path writes — it errs toward uploading.
//
// FLAGS
//   DC2_G590_ZWHY=1      [G590:zwhy] census: of the pages that moved, how many are INSIDE the
//                        uploaded window? `movedIn=0` is the share this lever can skip exactly.
//   DC2_G590_ZWIN=1      arm the lever (DEFAULT-OFF in this slice)
//   DC2_G590_NO_ZWIN=1   rollback once promoted
//   DC2_G590_ZVERIFY=1   same-run oracle: on every SKIP, build the upload anyway, read the
//                        resident texture back and compare. Implies the lever. Never valid for
//                        timing — it does strictly more work than either arm.
//   DC2_G419_AB=zwin     paired within-process arm.

// `g590ZWinArm` is DEFINED with external linkage in g419_ab_instrument.inc and DECLARED at global
// scope in ps2_gs_rasterizer.cpp. It must NOT be re-declared here: this file sits inside an
// anonymous namespace, so a declaration here would name a different, never-defined symbol
// (the G568 lesson — the same note guards g589DrainMemoArm).

static bool g590ZWhyOn()
{
    static const bool on = envFlagEnabled("DC2_G590_ZWHY");
    return on;
}

static bool g590ZVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G590_ZVERIFY");
    return on;
}

static bool g590ZPaired()
{
    static const bool paired = []() {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "zwin") == 0;
    }();
    return paired;
}

// ENV-LEVEL selection, arm-INDEPENDENT. Everything that must be symmetric between the two paired
// buckets — the page snapshot maintenance at the two stamp sites and the census — keys off this,
// so the control arm maintains the identical state and only the SKIP differs (G520 §7b).
static bool g590ZWinSelected()
{
    static const bool on = (envFlagEnabled("DC2_G590_ZWIN") || g590ZVerifyOn() || g590ZPaired()) &&
                           !envFlagEnabled("DC2_G590_NO_ZWIN");
    return on;
}

// Per-call: honours the paired arm, which flips every A/B tick and must never be cached.
static bool g590ZWinOn()
{
    if (!g590ZWinSelected())
        return false;
    if (g590ZPaired())
        return g590ZWinArm() > 0;
    return true;
}

// Any path that needs the per-page snapshot to exist at all.
static bool g590ZTracked()
{
    static const bool on = g590ZWhyOn() || g590ZWinSelected();
    return on;
}

// A 512x512 CT32/Z24 surface is 8 pages wide x 16 page rows = 128 pages — the same shape the
// colour side already snapshots in `G261Res::g536PageGen`.
static constexpr uint32_t kG590ZPages = 128u;
struct G590ZAuth
{
    uint32_t zbpPage = 0u, fbw = 0u;
    bool valid = false;
    std::array<uint64_t, kG590ZPages> gen{};
};
static std::unordered_map<uint64_t, G590ZAuth> g_g590ZAuth;

// Page slot walk shared by the stamp and the query, so the two can never disagree about which
// pages belong to a row band. Carries g178BumpRectImpl's +1 slack page (one extra slot past the
// band) unless the caller asks for the whole surface.
static inline uint32_t g590ZSlotLo(int rowLo, uint32_t fbw)
{
    return (static_cast<uint32_t>(std::max(0, rowLo)) / 32u) * fbw;
}
static inline uint32_t g590ZSlotHi(int rowHi, uint32_t fbw)
{
    return (static_cast<uint32_t>(std::min(511, rowHi)) / 32u) * fbw + fbw; // +1 slack slot
}

// Stamp the whole-surface page generations. Called at exactly the instants G274 stamps its
// whole-target baseline — i.e. straight after a full GPU->VRAM depth writeback, the only moment
// the resident texture is known to equal guest VRAM everywhere.
static void g590ZStamp(uint64_t depthKey, uint32_t zbpBlock, uint32_t fbw)
{
    if (!g590ZTracked() || depthKey == 0u || fbw == 0u)
        return;
    G590ZAuth &a = g_g590ZAuth[depthKey];
    a.zbpPage = zbpBlock >> 5;
    a.fbw = fbw;
    a.valid = true;
    const uint32_t slots = std::min(kG590ZPages, 16u * fbw);
    for (uint32_t s = 0; s < kG590ZPages; ++s)
        a.gen[s] = (s < slots) ? g_g178PageGen[(a.zbpPage + s) & (kG178PageCount - 1u)].load(
                                     std::memory_order_relaxed)
                               : 0u;
}

static void g590ZDropStamp(uint64_t depthKey)
{
    if (!g590ZTracked())
        return;
    auto it = g_g590ZAuth.find(depthKey);
    if (it != g_g590ZAuth.end())
        it->second.valid = false;
}

// ---- the census: of the pages that moved, how many are INSIDE the uploaded window? -------------
static std::atomic<uint64_t> g_g590ZWhyN{0}, g_g590ZWhyNoStamp{0}, g_g590ZWhyCleanIn{0},
    g_g590ZWhyDirtyIn{0}, g_g590ZWhyMovedIn{0}, g_g590ZWhyMovedOut{0};
static std::atomic<uint64_t> g_g590ZWhySrc[32]{};
static std::atomic<uint64_t> g_g590ZWhyBlk[6]{}; // prevKnown/fullInit/tight/seeded/priv/alreadySkip
static const char *const kG590ZBlkName[6] = {"noPrevGen", "fullInit", "tight",
                                             "seeded",    "privMir",  "alreadySkip"};

static void g590ZWhyPrint(uint64_t n)
{
    char blk[192];
    int bo = 0;
    for (int i = 0; i < 6; ++i)
    {
        const uint64_t v = g_g590ZWhyBlk[i].load(std::memory_order_relaxed);
        if (v != 0u)
            bo += std::snprintf(blk + bo, sizeof(blk) - bo, " %s=%llu", kG590ZBlkName[i],
                                static_cast<unsigned long long>(v));
    }
    blk[bo] = '\0';
    char src[256];
    int so = 0;
    for (int i = 0; i < 32; ++i)
    {
        const uint64_t v = g_g590ZWhySrc[i].load(std::memory_order_relaxed);
        if (v != 0u && so < 200)
            so += std::snprintf(src + so, sizeof(src) - so, " src%d=%llu", i,
                                static_cast<unsigned long long>(v));
    }
    src[so] = '\0';
    const uint64_t clean_ = g_g590ZWhyCleanIn.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G590:zwhy] n=%llu noStamp=%llu cleanWindow=%llu (%.2f%% SKIPPABLE) "
                 "dirtyWindow=%llu movedIn=%llu movedOut=%llu | blocked:%s | bumpSrc:%s\n",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_g590ZWhyNoStamp.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(clean_), 100.0 * (double)clean_ / (double)n,
                 static_cast<unsigned long long>(g_g590ZWhyDirtyIn.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZWhyMovedIn.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZWhyMovedOut.load(std::memory_order_relaxed)),
                 bo ? blk : " none", so ? src : " none");
}

// ---- the lever's counters ----------------------------------------------------------------------
static std::atomic<uint64_t> g_g590ZWinCalls{0}, g_g590ZWinSkips{0}, g_g590ZWinNoStamp{0},
    g_g590ZWinMoved{0};
static std::atomic<uint64_t> g_g590ZVfyChecks{0}, g_g590ZVfyPixels{0}, g_g590ZVfyBad{0},
    g_g590ZVfyFail{0};

// TRUE iff every page overlapping rows `rowLo..rowHi` of this Z surface is unchanged since the
// stamp, i.e. the resident depth texture is still authoritative for exactly those rows.
static bool g590ZWindowClean(uint64_t depthKey, uint32_t zbpBlock, uint32_t fbw, int rowLo,
                             int rowHi, uint32_t *movedInOut, uint32_t *movedOutOut)
{
    if (movedInOut != nullptr)
        *movedInOut = 0u;
    if (movedOutOut != nullptr)
        *movedOutOut = 0u;
    auto it = g_g590ZAuth.find(depthKey);
    if (it == g_g590ZAuth.end() || !it->second.valid)
        return false;
    G590ZAuth &a = it->second;
    if (a.fbw != fbw || a.zbpPage != (zbpBlock >> 5) || fbw == 0u || rowHi < rowLo)
        return false;
    const uint32_t slots = std::min(kG590ZPages, 16u * fbw);
    const uint32_t inLo = g590ZSlotLo(rowLo, fbw);
    const uint32_t inHi = std::min(slots - 1u, g590ZSlotHi(rowHi, fbw));
    uint32_t movedIn = 0u, movedOut = 0u;
    for (uint32_t s = 0; s < slots; ++s)
    {
        const uint64_t now =
            g_g178PageGen[(a.zbpPage + s) & (kG178PageCount - 1u)].load(std::memory_order_relaxed);
        if (now == a.gen[s])
            continue;
        if (s >= inLo && s <= inHi)
            ++movedIn;
        else
            ++movedOut;
    }
    if (movedInOut != nullptr)
        *movedInOut = movedIn;
    if (movedOutOut != nullptr)
        *movedOutOut = movedOut;
    return movedIn == 0u;
}

// Census entry point, called BEFORE the gate for every depth upload that actually happens (g513:
// take the census before the gate, so "the lever never fired" is separable from "the lever fired
// and found nothing"). Every conjunct of the gate is passed in and ranked, so a lever that is
// structurally unreachable names the conjunct that blocks it instead of printing silence — G589's
// slice-1 memo cost a whole phase by being mis-sited with no such counter.
// Also names the WRITER of each moved in-window page through the existing `[G570]` bump trace when
// DC2_G570_BUMP_TRACE=1 is armed alongside.
static void g590ZWhy(uint64_t depthKey, uint32_t zbpBlock, uint32_t fbw, int rowLo, int rowHi,
                     bool prevKnown, bool fullInit, bool tight, bool seeded, bool privateMirror,
                     bool alreadySkip)
{
    if (!g590ZWhyOn())
        return;
    const bool blocked[6] = {!prevKnown, fullInit, tight, seeded, privateMirror, alreadySkip};
    bool anyBlocked = false;
    for (int i = 0; i < 6; ++i)
        if (blocked[i])
        {
            g_g590ZWhyBlk[i].fetch_add(1u, std::memory_order_relaxed);
            anyBlocked = true;
        }
    if (anyBlocked)
    {
        // Still count the event so the SKIPPABLE share below has the true denominator.
        const uint64_t n = g_g590ZWhyN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if ((n % 512u) == 0u)
            g590ZWhyPrint(n);
        return;
    }
    uint32_t movedIn = 0u, movedOut = 0u;
    auto it = g_g590ZAuth.find(depthKey);
    const bool haveStamp = it != g_g590ZAuth.end() && it->second.valid;
    const bool clean = g590ZWindowClean(depthKey, zbpBlock, fbw, rowLo, rowHi, &movedIn, &movedOut);
    const uint64_t n = g_g590ZWhyN.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (!haveStamp)
        g_g590ZWhyNoStamp.fetch_add(1u, std::memory_order_relaxed);
    else if (clean)
        g_g590ZWhyCleanIn.fetch_add(1u, std::memory_order_relaxed);
    else
        g_g590ZWhyDirtyIn.fetch_add(1u, std::memory_order_relaxed);
    g_g590ZWhyMovedIn.fetch_add(movedIn, std::memory_order_relaxed);
    g_g590ZWhyMovedOut.fetch_add(movedOut, std::memory_order_relaxed);
    if (haveStamp && movedIn != 0u && g570BumpTraceOn())
    {
        const G590ZAuth &a = it->second;
        const uint32_t slots = std::min(kG590ZPages, 16u * fbw);
        const uint32_t inLo = g590ZSlotLo(rowLo, fbw);
        const uint32_t inHi = std::min(slots - 1u, g590ZSlotHi(rowHi, fbw));
        for (uint32_t s = inLo; s <= inHi; ++s)
        {
            const uint32_t page = (a.zbpPage + s) & (kG178PageCount - 1u);
            if (g_g178PageGen[page].load(std::memory_order_relaxed) == a.gen[s])
                continue;
            g_g590ZWhySrc[g_g570LastBump[page].source & 31u].fetch_add(1u,
                                                                      std::memory_order_relaxed);
        }
    }
    if ((n % 512u) == 0u)
        g590ZWhyPrint(n);
}

// The lever. Returns TRUE when the windowed depth upload can be skipped exactly.
static bool g590ZWinSkip(uint64_t depthKey, uint32_t zbpBlock, uint32_t fbw, int rowLo, int rowHi)
{
    if (!g590ZWinOn())
        return false;
    g_g590ZWinCalls.fetch_add(1u, std::memory_order_relaxed);
    uint32_t movedIn = 0u, movedOut = 0u;
    auto it = g_g590ZAuth.find(depthKey);
    if (it == g_g590ZAuth.end() || !it->second.valid)
    {
        g_g590ZWinNoStamp.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (!g590ZWindowClean(depthKey, zbpBlock, fbw, rowLo, rowHi, &movedIn, &movedOut))
    {
        g_g590ZWinMoved.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    g_g590ZWinSkips.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

// Same-run oracle for the SKIP decision: the caller built the upload anyway, so read the resident
// texture back and prove it already holds those values. `checks=` is printed beside `bad=` because
// a run that compared nothing must never look like a run that passed.
static void g590ZVerifySkip(uint64_t depthKey, int fbW, int fbH, int glY, int rows,
                            const std::vector<float> &upload)
{
    if (!g590ZVerifyOn() || rows <= 0 || fbW <= 0 || depthKey == 0u)
        return;
    static std::vector<float> s_z; // flush-owner thread only
    if (!g242_backend_read_depth(depthKey, fbW, fbH, glY, rows, s_z) ||
        s_z.size() != static_cast<size_t>(fbW) * static_cast<size_t>(rows) ||
        upload.size() < s_z.size())
    {
        g_g590ZVfyFail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t bad = 0;
    for (size_t i = 0, n = s_z.size(); i < n; ++i)
        if (s_z[i] != upload[i])
            ++bad;
    const uint64_t checks = g_g590ZVfyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint64_t px =
        g_g590ZVfyPixels.fetch_add(s_z.size(), std::memory_order_relaxed) + s_z.size();
    const uint64_t totBad = g_g590ZVfyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0u || checks <= 8u || (checks % 512u) == 0u)
        std::fprintf(stderr,
                     "[G590:zvfy] checks=%llu pixels=%llu bad=%llu readFail=%llu "
                     "last(key=%llx rows=%d glY=%d bad=%llu)\n",
                     static_cast<unsigned long long>(checks), static_cast<unsigned long long>(px),
                     static_cast<unsigned long long>(totBad),
                     static_cast<unsigned long long>(
                         g_g590ZVfyFail.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(depthKey), rows, glY,
                     static_cast<unsigned long long>(bad));
}

static void g590ZStat()
{
    const uint64_t calls = g_g590ZWinCalls.load(std::memory_order_relaxed);
    if (calls == 0u)
        return;
    const uint64_t skips = g_g590ZWinSkips.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G590:zwin] calls=%llu skips=%llu (%.2f%%) noStamp=%llu moved=%llu "
                 "vfy(checks=%llu pixels=%llu bad=%llu readFail=%llu)\n",
                 static_cast<unsigned long long>(calls), static_cast<unsigned long long>(skips),
                 100.0 * (double)skips / (double)calls,
                 static_cast<unsigned long long>(g_g590ZWinNoStamp.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZWinMoved.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZVfyChecks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZVfyPixels.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZVfyBad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g590ZVfyFail.load(std::memory_order_relaxed)));
}

// Final report at teardown, so a run that ends between cadence points still yields its totals.
static void g590Report()
{
    g590ZStat();
    if (!g590Armed())
        return;
    g590Print();
}
