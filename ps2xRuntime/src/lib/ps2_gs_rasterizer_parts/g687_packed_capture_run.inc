// G687 - fixed-state capture run for the dominant PACKED ST/RGBAQ/XYZF2 tag (0x412).
//
// The GIF parser still executes writeRegisterPacked for every descriptor, preserving vertex-queue
// and register behavior exactly. A normal drawPrimitive call must first reach the proven G144
// defer exit. That call establishes every stateful front-door effect and primes this TLS snapshot.
// Later TRISTRIP kicks in the same tag differ only in their vertices, so this compact path repeats
// the vertex-dependent cull and constructs the identical G144Entry without re-running invariant
// eligibility, target discovery, target-switch, and diagnostic-front-door work.
//
// ⭐ DEFAULT-ON, promoted by G688 (-0.173 ms/f, exact over 10.1 M tags). Rollback:
// DC2_G687_NO_PACKED_CAPTURE_RUN=1. See the promotion block on `enabled` below for the gates.
// DC2_G687_VERIFY_PACKED_CAPTURE=1 checks every invariant state byte and appended entry.
// DC2_G687_STAT=1 reports coverage. ⚠️ Both of those cost per TAG — never in a timing run (Rule 63).

struct G687PackedCaptureState
{
    bool primed = false;
    bool verify = false;
    bool stat = false;
    bool g286Transient = false;
    bool g573Tri13b = false;
    bool pabe = false;
    GSRasterizer *self = nullptr;
    GS *gs = nullptr;
    uint8_t *vram = nullptr;
    unsigned char ctxBytes[sizeof(GSContext)]{};
    unsigned char primBytes[sizeof(GSPrimReg)]{};
    unsigned char texaBytes[sizeof(GSTexaReg)]{};
    unsigned char texclutBytes[sizeof(GSTexClutReg)]{};
};

struct G687PackedCaptureStats
{
    uint64_t tags = 0;
    uint64_t primedTags = 0;
    uint64_t fastEntries = 0;
    uint64_t culledEntries = 0;
    uint64_t fallbacks = 0;
};

static thread_local G687PackedCaptureState t_g687PackedCapture;
static thread_local G687PackedCaptureStats t_g687PackedStats;

static bool g687RuntimeQuiet()
{
    static const bool quiet =
        std::getenv("DC2_PERF") == nullptr &&
        std::getenv("DC2_G156_STAT") == nullptr &&
        std::getenv("DC2_G219_SKYPX") == nullptr &&
        std::getenv("DC2_G312_STAT") == nullptr &&
        std::getenv("DC2_G369_CENSUS") == nullptr &&
        std::getenv("DC2_G419_AB") == nullptr &&
        std::getenv("DC2_G458_STAT") == nullptr &&
        std::getenv("DC2_G459_STAT") == nullptr &&
        std::getenv("DC2_G519_VERIFY") == nullptr &&
        std::getenv("DC2_G521_SIZE") == nullptr &&
        std::getenv("DC2_G526_DRAW") == nullptr &&
        std::getenv("DC2_G527_STAT") == nullptr &&
        std::getenv("DC2_G595_PRO") == nullptr &&
        !phaseDiagnosticsEnabled() && !renderQualityTraceEnabled() &&
        !f50_12_trace_enabled() && !ps2_log::agressive_logs_enabled;
    return quiet;
}

bool g687BeginPackedCaptureRun()
{
#if defined(PS2X_G401_DIAG) || defined(PS2X_G654_DIAG) || \
    defined(PS2X_G655_LEGACY_DIAG) || DC2_G521_INPLACEARM
    return false;
#else
    // ⭐ G688 PROMOTION (2026-09-02): DEFAULT-ON, after the gates G687 never ran.
    //
    // ⚠️ It had shipped default-ON in the working tree with NO recorded gate, while
    // `plans/phase-G687-fix-log.md` §7 recorded "Source Code: Unchanged" and `dc2_runner.exe` had
    // silently drifted 53,248 B off the promoted SHA (G688 §1, Rule 62). G688 demoted it to opt-in,
    // restored the deliverable, then gated it properly. The evidence it now carries:
    //
    //   * EXACT: `[G687:run] fallback=0` over 10,158,080 tags / 13,694,236 fast entries on
    //     `dungeon1` — the fixed-state-tag invariant never broke once. Oracle
    //     DC2_G687_VERIFY_PACKED_CAPTURE=1 compares every invariant state byte and appended entry.
    //   * TIMING: same-binary A B B A on `dungeon1` (kill flag as the control arm, so both arms
    //     share one binary and one layout): total 22.5015 -> 22.3290 = **-0.173 ms/f**, front
    //     14.405 -> 14.211 = **-0.194 ms/f**, backend unmoved (+0.025 — the lever is front-end
    //     only, as designed). Control drift 0.043 ms/f against the Rule 16 0.30 envelope, arm
    //     ranges NON-OVERLAPPING (A 22.480–22.523 vs B 22.305–22.353), all four blocks consistent.
    //   * ⚠️ That number is a LOWER bound: it was measured on a relink whose PGO profile predates
    //     this file, so these functions were unprofiled in the measured binary.
    //
    // ⛔ It is NOT the G687 "Pathway A" prize. Skipping the whole front door for 13.7 M entries
    // buys 0.194 ms/f, i.e. ~8% of the ~2.5 ms/f that decomposition predicted — so the per-primitive
    // capture cost is NOT mostly front-door logic, and a full `G144Entry` bypass (which adds only
    // the entry write, itself pre-refuted by G521's 0.000957 ms/f-per-byte slope) has an absolute
    // ceiling near 0.44 ms/f. Pathway A is refuted at the ceiling; see G688 §9.
    //
    // Rollback: DC2_G687_NO_PACKED_CAPTURE_RUN=1 (or DC2_G687_PACKED_CAPTURE_RUN=0).
    static const bool enabled =
        std::getenv("DC2_G687_NO_PACKED_CAPTURE_RUN") == nullptr &&
        !envFlagDisabled("DC2_G687_PACKED_CAPTURE_RUN");
    if (!enabled || !g687RuntimeQuiet() || g_g687PackedCaptureRunActive)
        return false;

    G687PackedCaptureState &run = t_g687PackedCapture;
    run.primed = false;
    static const bool verify = std::getenv("DC2_G687_VERIFY_PACKED_CAPTURE") != nullptr;
    static const bool stat = std::getenv("DC2_G687_STAT") != nullptr;
    run.verify = verify;
    run.stat = stat;
    run.g286Transient = false;
    run.g573Tri13b = false;
    run.pabe = false;
    run.self = nullptr;
    run.gs = nullptr;
    run.vram = nullptr;
    g_g687PackedCaptureRunActive = true;
    if (run.stat)
        ++t_g687PackedStats.tags;
    return true;
#endif
}

void g687EndPackedCaptureRun()
{
    if (!g_g687PackedCaptureRunActive)
        return;

    if (t_g687PackedCapture.stat && t_g687PackedCapture.primed)
        ++t_g687PackedStats.primedTags;
    g_g687PackedCaptureRunActive = false;
    t_g687PackedCapture.primed = false;

    const uint64_t tags = t_g687PackedStats.tags;
    if (t_g687PackedCapture.stat && (tags <= 8u || (tags & 0x3FFFull) == 0u))
    {
        std::fprintf(stderr,
                     "[G687:run] tags=%llu primed=%llu fast=%llu culled=%llu fallback=%llu\n",
                     static_cast<unsigned long long>(tags),
                     static_cast<unsigned long long>(t_g687PackedStats.primedTags),
                     static_cast<unsigned long long>(t_g687PackedStats.fastEntries),
                     static_cast<unsigned long long>(t_g687PackedStats.culledEntries),
                     static_cast<unsigned long long>(t_g687PackedStats.fallbacks));
    }
}

static void g687PrimePackedCapture(
    GSRasterizer *self, GS *gs, uint8_t *vram, const GSContext &ctx,
    const GSPrimReg &prim, const GSTexaReg &texa, const GSTexClutReg &texclut,
    bool pabe, bool g286Transient, bool g573Tri13b)
{
    if (!g_g687PackedCaptureRunActive || t_g687PackedCapture.primed ||
        prim.type != GS_PRIM_TRISTRIP)
        return;

    G687PackedCaptureState &run = t_g687PackedCapture;
    run.self = self;
    run.gs = gs;
    run.vram = vram;
    run.pabe = pabe;
    run.g286Transient = g286Transient;
    run.g573Tri13b = g573Tri13b;
    if (run.verify)
    {
        std::memcpy(run.ctxBytes, &ctx, sizeof(ctx));
        std::memcpy(run.primBytes, &prim, sizeof(prim));
        std::memcpy(run.texaBytes, &texa, sizeof(texa));
        std::memcpy(run.texclutBytes, &texclut, sizeof(texclut));
    }
    run.primed = true;
}

static bool g687StateChanged(const G687PackedCaptureState &run, const GSContext &ctx,
                             const GSPrimReg &prim, const GSTexaReg &texa,
                             const GSTexClutReg &texclut, bool pabe)
{
    return std::memcmp(run.ctxBytes, &ctx, sizeof(ctx)) != 0 ||
           std::memcmp(run.primBytes, &prim, sizeof(prim)) != 0 ||
           std::memcmp(run.texaBytes, &texa, sizeof(texa)) != 0 ||
           std::memcmp(run.texclutBytes, &texclut, sizeof(texclut)) != 0 ||
           run.pabe != pabe;
}

__declspec(noinline) static bool g687TryFastCapture(
    GSRasterizer *self, GS *gs, const GSContext &ctx, const GSPrimReg &prim,
    const GSTexaReg &texa, const GSTexClutReg &texclut, bool pabe,
    const GSVertex *vertices, uint8_t *vram)
{
    G687PackedCaptureState &run = t_g687PackedCapture;
    if (!run.primed)
        return false;

    // A fixed-state tag cannot reach any of these conditions. Fail back to the complete path if a
    // future parser/queue change violates that premise; the optional oracle makes it fatal.
    const bool stateChanged = run.verify && g687StateChanged(run, ctx, prim, texa, texclut, pabe);
    const bool structuralBreak = self != run.self || gs != run.gs || vram != run.vram ||
                                 prim.type != GS_PRIM_TRISTRIP || g_g144List.empty() ||
                                 ctx.frame.fbp != g_g144BlockFbp;
    if (stateChanged || structuralBreak)
    {
        if (run.verify)
        {
            std::fprintf(stderr,
                         "[G687:verify] invariant failure state=%u structure=%u fbp=0x%x block=0x%x\n",
                         stateChanged ? 1u : 0u, structuralBreak ? 1u : 0u,
                         ctx.frame.fbp, g_g144BlockFbp);
            std::fflush(stderr);
            std::abort();
        }
        run.primed = false;
        if (run.stat)
            ++t_g687PackedStats.fallbacks;
        return false;
    }

    // Exact G458 three-vertex scissor rejection. G459 is diagnostic-only and begin() refuses it.
    if (g458CullClippedOn())
    {
        const float minX = std::min({vertices[0].x, vertices[1].x, vertices[2].x});
        const float maxX = std::max({vertices[0].x, vertices[1].x, vertices[2].x});
        const float minY = std::min({vertices[0].y, vertices[1].y, vertices[2].y});
        const float maxY = std::max({vertices[0].y, vertices[1].y, vertices[2].y});
        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        const float cx = maxX - ofx;
        const float fx = minX - ofx;
        const float cy = maxY - ofy;
        const float fy = minY - ofy;
        const float sx0 = static_cast<float>(ctx.scissor.x0);
        const float sx1 = static_cast<float>(ctx.scissor.x1);
        const float sy0 = static_cast<float>(ctx.scissor.y0);
        const float sy1 = static_cast<float>(ctx.scissor.y1);
        const int g519Arm = g519CullCmpArm();
        const bool useCmp = (g519Arm >= 0) ? (g519Arm == 1) : g519CullCmpDefaultOn();
        const bool misses = useCmp
                                ? (cx <= sx0 - 1.0f || fx >= sx1 + 1.0f ||
                                   cy <= sy0 - 1.0f || fy >= sy1 + 1.0f)
                                : (std::ceil(cx) < sx0 || std::floor(fx) > sx1 ||
                                   std::ceil(cy) < sy0 || std::floor(fy) > sy1);
        if (misses)
        {
            if (run.stat)
                ++t_g687PackedStats.culledEntries;
            return true;
        }
    }

    const int cofy = ctx.xyoffset.ofy >> 4;
    const float fy0 = vertices[0].y - static_cast<float>(cofy);
    const float fy1 = vertices[1].y - static_cast<float>(cofy);
    const float fy2 = vertices[2].y - static_cast<float>(cofy);
    const int rawMinY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    const int rawMaxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));
    const bool titleRockScope = g_dc2TitleRockScope.load(std::memory_order_relaxed);
    const bool townDepthScope = g_dc2TownDepthScope.load(std::memory_order_relaxed);
    const uint32_t fogcol = g_g364FogCol;

    G144Entry e;
    e.v[0] = vertices[0];
    e.v[1] = vertices[1];
    e.v[2] = vertices[2];
    e.ctx = ctx;
    e.prim = prim;
    e.texa = texa;
    e.texclut = texclut;
    e.pabe = pabe;
    e.fogcol = fogcol;
    e.titleRockScope = titleRockScope;
    e.townDepthScope = townDepthScope;
    e.minY = clampInt(rawMinY, ctx.scissor.y0, ctx.scissor.y1);
    e.maxY = clampInt(rawMaxY, ctx.scissor.y0, ctx.scissor.y1);
    e.decoded = g149BuildDecoded(self, gs, ctx, texa, vram);

    g161CensusEligibility(ctx, prim.abe);
    if (run.g286Transient)
        g_g286Captured.fetch_add(1u, std::memory_order_relaxed);
    if (run.g573Tri13b)
        g573NoteTri13bAdmission(static_cast<uint32_t>(prim.type));
    if (g260NrOn())
        g595NoteAppend(e);
    g336NoteConsumer(e);

    const size_t oldSize = run.verify ? g_g144List.size() : 0u;
    const G149Tex *const decoded = run.verify && e.decoded ? e.decoded.get() : nullptr;
    if (g510CapMoveActive())
        g_g144List.push_back(std::move(e));
    else
        g_g144List.push_back(e);

    if (run.verify)
    {
        const G144Entry &slot = g_g144List.back();
        const bool bad = g_g144List.size() != oldSize + 1u ||
                         std::memcmp(&slot.ctx, &ctx, sizeof(ctx)) != 0 ||
                         std::memcmp(&slot.prim, &prim, sizeof(prim)) != 0 ||
                         std::memcmp(&slot.texa, &texa, sizeof(texa)) != 0 ||
                         std::memcmp(&slot.texclut, &texclut, sizeof(texclut)) != 0 ||
                         std::memcmp(slot.v, vertices, sizeof(GSVertex) * 3u) != 0 ||
                         slot.pabe != pabe || slot.fogcol != fogcol ||
                         slot.titleRockScope != titleRockScope ||
                         slot.townDepthScope != townDepthScope ||
                         slot.minY != clampInt(rawMinY, ctx.scissor.y0, ctx.scissor.y1) ||
                         slot.maxY != clampInt(rawMaxY, ctx.scissor.y0, ctx.scissor.y1) ||
                         slot.decoded.get() != decoded;
        if (bad)
        {
            std::fprintf(stderr, "[G687:verify] captured entry mismatch at fast=%llu\n",
                         static_cast<unsigned long long>(t_g687PackedStats.fastEntries));
            std::fflush(stderr);
            std::abort();
        }
    }

    if (run.stat)
        ++t_g687PackedStats.fastEntries;
    return true;
}
