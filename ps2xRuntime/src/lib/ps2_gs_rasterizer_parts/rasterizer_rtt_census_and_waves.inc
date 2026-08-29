// ===== G262 RTT-family classifier reject census (opt-in, DC2_G262_CENSUS=1) =====
// G261's rttRej(classify=1836) lumps every g178ClassifyEntry reject site together, and the
// wave-level depth counter is only reachable under DC2_G242_GPU_DEPTH (waves decline it), so
// "depth=0" cannot distinguish state-shape rejects from the universal-Z depth reject INSIDE
// classify. This census splits the sites and logs deduplicated state shapes for the five RTT
// targets only, so the widening decision attacks the real dominant shape.
enum G262RejSite : int
{
    kG262RejFrame = 0,   // frame psm/fbmsk/fbw/fba/pabe
    kG262RejATest = 1,   // DATE or non-no-op alpha test
    kG262RejScissor = 2, // scissor sanity/bounds
    kG262RejBlend = 3,   // unsupported ALPHA a/b/c/d combo
    kG262RejTex = 4,     // tex psm/tbw/wrap/size
    kG262RejZPsm = 5,    // G242 path, non-Z24 zpsm
    kG262RejDepth = 6,   // universal-Z real Z test/write without GPU depth
    kG262RejLegacyZ = 7, // legacy town/title Z scopes
    kG262RejSiteCount = 8
};
static const char *const kG262SiteName[kG262RejSiteCount] = {
    "frame", "atest", "scissor", "blend", "tex", "zpsm", "depth", "legacyz"};
static std::atomic<uint64_t> g_g262Rej[kG262RejSiteCount] = {};
static std::atomic<uint64_t> g_g262RttSeen{0};
// Shape map is only touched on the flush-owning thread (same ownership as g_g178FbSnap).
static std::map<uint64_t, uint64_t> g_g262Shapes;

// G265 display reject census. Keep it separate from G262's RTT-only counters so enabling the
// diagnostic cannot change the historical RTT denominator or shape reports.
static std::atomic<uint64_t> g_g265Seen{0};
static std::atomic<uint64_t> g_g265Rej[kG262RejSiteCount] = {};
static std::unordered_set<uint64_t> g_g265Shapes;
static std::atomic<uint64_t> g_g265DepthBatches{0}, g_g265ZReadbacks{0};

static bool g265DisplayTarget(uint32_t fbp)
{
    return fbp == 0u || fbp == 0x68u;
}

// ===== G662 P2: the `colorzalias` population that dies one CLASSIFIER STAGE EARLIER ============
//
// G661 P4's 14-route wall census found the corpus's only top-three residual family that is NOT
// `cpuonlyrtt cls=discov`: `map125` carries **7,934.3 ms (47.14% of that route's whole CPU-fallback
// wall)** of `rej=classify site=blend cls=legacy fbp=0x139`, and `map125` is also the corpus's
// WORST GPU-native WALL coverage (63.12%). G660 promoted the 0x139 admission via the
// `g570Exact139` private-depth carve-out — but that carve-out is consulted at the colour/Z ALIAS
// stage, and these entries never get there: they are rejected at `site=blend`, above.
//
// The reason is scope, not capability. Blend combos 5 (`(0-Cd)*As>>7 + Cd`) and 6
// (`(0-Cs)*As>>7 + Cd`) are admitted only on `g265DisplayTarget`, which is literally
// `fbp == 0 || fbp == 0x68`. The BACKEND has no such restriction: its blend dispatch
// (lle_gpu_raster_backend.inc, the g496BlendOn call) is a per-draw decision on `d.blend` alone,
// and combos 5/6 already run there every frame for the display buffers. The `d.blend & 3` packing
// that genuinely cannot represent a 6 belongs to the g256Exact program, and `g256ExactRttOn()`
// needs two env flags and is off on every default and every perf run.
//
// So this widens exactly one predicate, and only for the address whose exact contract G660 already
// promoted and gated. It fails CLOSED: without `g570Private139On()` the carve-out that admits the
// batch downstream is not armed either, so admitting it here would only trade a `blend` reject for
// a `colorzalias` one.
//
// ⛔ NO `DC2_G419_AB` ARM, for the same reason as G603/G604: this is read by the CLASSIFIER, so it
// changes WHICH authority produced the pixels and it changes the batching population (G657
// measurement law 9). Measure it with alternating WHOLE RUNS on a scripted route.
// ⭐ PROMOTED TO DEFAULT-ON (G662 P2). Rollback: DC2_G662_NO_BLEND139=1.
//
// All four gates passed, on `map125`, at matched denominators:
//   WALL      cpuMs 16,649.0 -> 8,644.6 at gpuBatches=159,744 (-48.1%), and the
//             `rej=classify site=blend cls=legacy fbp=139` row (46.17% of that route's fallback
//             wall) VANISHES from `[G657:rejwall]` entirely.
//   COVERAGE  GPU-native WALL 63.31% -> 77.87% (+14.56 points) on the corpus's WORST-covered route.
//   EXACTNESS `[G570:verify139]` RGB 0.071% of the compare rect, cov 0.030%, and 65% of the raw
//             `bad` is the standing GL-0..255 vs PS2-raw-0..128 ALPHA convention (first divergence
//             is cpu=00000000 gpu=40000000 — RGB identical). The shipped display path this joins
//             reads 1.2275%-70.0123% at matched write depth (G660 law 14).
//   PIXELS    `g635_bisect` p50/p90/max 2.80/4.68/7.15 against a SAME-BINARY control floor of
//             0.00/4.07/5.02; `g635_look` on both worst keys shows the identical signature — the
//             route's own animated starfield particles — with 0/3264 tiles over 20 in BOTH, and
//             the character's drop shadow (G659's failure signature) present and unchanged.
//   TIMING    `map125:map` lifted off the 60 FPS cap with matched GS ballast in both arms:
//             frame 17.367 -> 17.036 = -0.331 ms/f (-1.91%), blocks -0.135 / -0.527 (SAME sign),
//             control drift 0.100; GS own -0.535.
static bool g662Blend139On()
{
    static const bool s_on = !envFlagEnabled("DC2_G662_NO_BLEND139") &&
                             !envFlagDisabled("DC2_G662_BLEND139");
    return s_on;
}

// Every site that asks "may this target take the destination-dependent display blend combos?".
// The `fbp == 0x139u` compare is FIRST so the default build pays exactly one extra integer test on
// the hot classifier path and never touches either magic-static guard (rule 12b).
static inline bool g662DestBlendTarget(uint32_t fbp)
{
    if (g265DisplayTarget(fbp))
        return true;
    if (fbp != 0x139u || !g662Blend139On() || !g570Private139On())
        return false;
#if defined(PS2X_G662_DIAG)
    // G657 measurement law 10 / G659 P18: an arm that is never observed cannot produce a measured
    // null. This ticks only on the entries the widening actually moved.
    g662NoteLever(kG662LeverBlend139);
#endif
    return true;
}

static void g265NoteSeen(const G144Entry &e)
{
    if (g265CensusOn() && g265DisplayTarget(e.ctx.frame.fbp))
        g_g265Seen.fetch_add(1u, std::memory_order_relaxed);
}

static void g265NoteReject(int site, const G144Entry &e)
{
    if (!g265CensusOn() || !g265DisplayTarget(e.ctx.frame.fbp))
        return;
    g_g265Rej[site].fetch_add(1u, std::memory_order_relaxed);
    const GSContext &ctx = e.ctx;
    const uint64_t ts = ctx.test;
    const uint64_t al = ctx.alpha;
    uint64_t key = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(site)); mix(ctx.frame.fbp); mix(e.prim.type);
    mix(e.prim.tme); mix(e.prim.abe); mix(ctx.frame.fbw); mix(ctx.frame.psm);
    mix(ctx.frame.fbmsk); mix(ctx.fba); mix(e.pabe); mix(ts); mix(ctx.zbuf); mix(al);
    mix(ctx.tex0.psm); mix(ctx.tex0.tbw); mix(ctx.clamp);
    const bool fresh = g_g265Shapes.insert(key).second;
    static std::atomic<uint32_t> s_log{0};
    if (fresh && s_log.fetch_add(1u, std::memory_order_relaxed) < 48u)
        std::fprintf(stderr,
                     "[G265:rej] site=%s fbp=%03x prim=%u tme=%u abe=%u abcd=%u%u%u%u "
                     "fix=%u ate=%u atst=%u aref=%u afail=%u date=%u tpsm=0x%x tbw=%u "
                     "wrap=%u%u zte=%u ztst=%u zmsk=%u zpsm=%u fbmskNZ=%u fba=%u pabe=%u\n",
                     kG262SiteName[site], ctx.frame.fbp, static_cast<unsigned>(e.prim.type),
                     e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                     static_cast<unsigned>(al & 3u), static_cast<unsigned>((al >> 2u) & 3u),
                     static_cast<unsigned>((al >> 4u) & 3u),
                     static_cast<unsigned>((al >> 6u) & 3u),
                     static_cast<unsigned>((al >> 32u) & 0xffu),
                     static_cast<unsigned>(ts & 1u), static_cast<unsigned>((ts >> 1u) & 7u),
                     static_cast<unsigned>((ts >> 4u) & 0xffu),
                     static_cast<unsigned>((ts >> 12u) & 3u),
                     static_cast<unsigned>((ts >> 14u) & 1u), ctx.tex0.psm,
                     static_cast<unsigned>(ctx.tex0.tbw),
                     static_cast<unsigned>(ctx.clamp & 3u),
                     static_cast<unsigned>((ctx.clamp >> 2u) & 3u),
                     static_cast<unsigned>((ts >> 16u) & 1u),
                     static_cast<unsigned>((ts >> 17u) & 3u),
                     static_cast<unsigned>((ctx.zbuf >> 32u) & 1u),
                     static_cast<unsigned>((ctx.zbuf >> 24u) & 0xfu),
                     ctx.frame.fbmsk != 0u ? 1u : 0u,
                     (ctx.fba & 1ull) != 0ull ? 1u : 0u, e.pabe ? 1u : 0u);
}

static void g265Report()
{
    if (!g265CensusOn())
        return;
    std::fprintf(stderr, "[G265:census] seen=%llu shapes=%zu rej(",
                 (unsigned long long)g_g265Seen.load(std::memory_order_relaxed),
                 g_g265Shapes.size());
    for (int i = 0; i < kG262RejSiteCount; ++i)
        std::fprintf(stderr, "%s%s=%llu", i ? " " : "", kG262SiteName[i],
                     (unsigned long long)g_g265Rej[i].load(std::memory_order_relaxed));
    std::fprintf(stderr, ") zwave=%llu zrb=%llu\n",
                 (unsigned long long)g_g265DepthBatches.load(std::memory_order_relaxed),
                 (unsigned long long)g_g265ZReadbacks.load(std::memory_order_relaxed));
}

static bool g262CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G262_CENSUS");
    return s_on;
}

static void g262Report()
{
    if (!g262CensusOn())
        return;
    std::fprintf(stderr, "[G262:census] seen=%llu rej(",
                 (unsigned long long)g_g262RttSeen.load(std::memory_order_relaxed));
    for (int i = 0; i < kG262RejSiteCount; ++i)
        std::fprintf(stderr, "%s%s=%llu", i ? " " : "", kG262SiteName[i],
                     (unsigned long long)g_g262Rej[i].load(std::memory_order_relaxed));
    std::fprintf(stderr, ")\n");
    for (const auto &kv : g_g262Shapes)
    {
        const uint64_t k = kv.first;
        static constexpr uint32_t kFbp[5] = {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
        std::fprintf(stderr,
                     "[G262:shape] n=%llu site=%s fbp=%03x prim=%u tme=%u abe=%u abcd=%u%u%u%u "
                     "fix=%u ate=%u atst=%u aref=%u afail=%u date=%u tpsm=0x%02x wu=%u wv=%u "
                     "zte=%u ztst=%u zmsk=%u zpsm=%u fbmskNZ=%u fba=%u pabe=%u\n",
                     (unsigned long long)kv.second,
                     kG262SiteName[(k >> 60) & 0x7u],
                     kFbp[(k >> 57) & 0x7u],
                     (unsigned)((k >> 54) & 0x7u),  // prim.type
                     (unsigned)((k >> 33) & 1u),    // tme
                     (unsigned)((k >> 34) & 1u),    // abe
                     (unsigned)((k >> 35) & 3u), (unsigned)((k >> 37) & 3u), // a,b
                     (unsigned)((k >> 39) & 3u), (unsigned)((k >> 41) & 3u), // c,d
                     (unsigned)((k >> 43) & 0xFFu), // fix
                     (unsigned)((k >> 18) & 1u),    // ate
                     (unsigned)((k >> 19) & 7u),    // atst
                     (unsigned)((k >> 22) & 0xFFu), // aref
                     (unsigned)((k >> 30) & 3u),    // afail
                     (unsigned)((k >> 32) & 1u),    // date
                     (unsigned)((k >> 8) & 0x3Fu),  // tex psm
                     (unsigned)((k >> 14) & 3u), (unsigned)((k >> 16) & 3u), // wrapU, wrapV
                     (unsigned)((k >> 1) & 1u),     // zte
                     (unsigned)((k >> 2) & 3u),     // ztst
                     (unsigned)(k & 1u),            // zmsk
                     (unsigned)((k >> 4) & 0xFu),   // zpsm
                     (unsigned)((k >> 51) & 1u),    // fbmsk != 0
                     (unsigned)((k >> 52) & 1u),    // fba
                     (unsigned)((k >> 53) & 1u));   // pabe
    }
}

// ===== G266 post-classify consumer census (opt-in, DC2_G266_CENSUS=1; diagnostics only) =====
// G265 closed the display classifier (MAP-0 rejects zero) yet 1,024-wave checkpoints still drain
// RTT residency through mat(cpu=237 l2l=151 tex=113). This census records, at the consumer edges
// themselves: (a) WHY the flushed batch that forces a CPU-replay materialize left the GPU path
// (flush reject reason + classify site) and what batch/overlap shape it is; (b) the exact
// source/destination roles and layouts of every local→local copy that drains a resident target;
// (c) the state shape of every rejected FBO-direct texture bind that still materializes.
// Shape maps are touched only on the flush-owner threads (same contract as g_g178FbSnap).
static bool g266CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G266_CENSUS");
    return s_on;
}
static bool g282CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_CENSUS");
    return s_on;
}
enum G266FlushRej : int
{
    kG266FrNone = 0,        // GPU attempt succeeded (or none recorded yet)
    kG266FrOff = 1,         // GPU path off / backend not ready / empty list
    kG266FrCpuOnlyRtt = 2,  // RTT target without G255 routing (G252 CPU-only rule)
    kG266FrFbW = 3,         // frame width out of range
    kG266FrMixedTgt = 4,    // mixed fbp/fbw inside one flush
    kG266FrClassify = 5,    // g178ClassifyEntry reject (site in g_g266ClassifySite)
    kG266FrMixedZ = 6,      // two guest-Z tuples in one flush
    kG266FrRttDepth = 7,    // RTT guest depth without G262 widening
    kG266FrColorZAlias = 8, // color rows alias the Z rows
    kG266FrDepthSetup = 9,  // guest-depth setup fallback (rows/overlap-sync/G242 invariant)
    kG266FrTexAliasZ = 10,  // texture/CLUT pages alias the live Z window (or their depth sync)
    kG266FrSubmit = 11,     // backend submit failed
    kG266FrZReadback = 12,  // per-wave Z readback failed
};
static int g_g266FlushRej = kG266FrNone; // why the LAST g178TryFlushGpu returned false
static int g_g266ClassifySite = -1;      // kG262Rej* site of the latest classify reject
// ===== G603: THE DISPLAY BUFFER'S SUBTRACTIVE BLEND =====
// `[G603:admit]` on route `s03` (the user's 2026-08-16 recording), 1,066 display CPU-fallback
// batches / 137,449 entries:
//
//   cls=disp fbp=000  batches=534  entries=68760  allOk=188  allOkRelaxBlend=534
//   cls=disp fbp=068  batches=532  entries=68689  allOk=187  allOkRelaxBlend=532
//   shipped(blend=23474 ok=113975)                relaxBlend(ok=137449)
//   [G603:blend] A=2 B=0 C=0 D=1 fix128=0 tpsm=0x13 entries=23474   <- ONE shape, 100% of them
//
// i.e. a SINGLE unimplemented ALPHA combo is the only thing keeping the display batch off the GPU,
// and with it admitted EVERY entry of EVERY one of those batches classifies clean — measured with
// the relax pass, not inferred (g534_admission_proof_true_but_insufficient).
//
// The combo is `((0 - Cs) * As) >> 7 + Cd` = `Cd - Cs*As/128`: source-alpha-weighted SUBTRACTIVE.
// It is blend mode 4 (`Cd - Cs`, FIX=128, already promoted) with As in place of the fixed 128, so
// the backend expresses it with the same GL_FUNC_REVERSE_SUBTRACT equation and the ordinary
// GL_SRC_ALPHA factor — and the fragment shader's alpha output already carries GS's /128 weight
// (`min(As*255/128, 1)`, G584), so this does NOT reintroduce the /255 factor error.
//
// SCOPE: display targets only, under the same g265 display-wave contract that gates mode 5. That
// keeps it disjoint from the g256Exact program (legacy five RTT fbps), whose `d.blend & 3` packing
// could not represent a 6.
//
// Bring-up default OFF (DC2_G603_SUBBLEND=1); rollback after promotion DC2_G603_NO_SUBBLEND=1.
// ⛔ NOT given a DC2_G419_AB arm: admitting the batch changes WHICH authority produced the display
// pixels and runs the g265 Z round-trip, so alternating it per frame would have the control frames
// composite over the candidate's output (g569_no_ab_arm_for_an_authority_lever). Measure it with
// alternating WHOLE RUNS on a scripted route, bucketed by window.
// ⭐ PROMOTED TO DEFAULT-ON (G659). Rollback: DC2_G603_NO_SUBBLEND=1.
//
// Held default-OFF by G603/G604/G657/G658 on one number: the same-run oracle's in-footprint
// `bigRgb` against a region control (59.09% vs 1.43% on `lavaboss`, a 41.3x ratio). G659's
// rewrite-depth census showed that ratio is an OVERDRAW-DEPTH CONFOUND -- the mode-6 footprint is
// the densest overdraw in the frame and the region control is about one write deep. Bucketed by how
// many times the CPU replay actually committed each pixel:
//   * pixels written ONLY by mode-6 draws diverge at 0.0000-2.60% with a max channel delta of 11
//     (`lavaboss`: 0.0000% and max delta 1 at depths 4 and above);
//   * the ordinary display draws this binary ALREADY admits diverge at 7.24-22.01% with a max
//     channel delta of 255;
//   * at MATCHED ordinary-overdraw depth, adding mode-6 writes does not raise the rate, and on
//     `lavaboss` it lowers it at five of six depths.
// Gates: `ridepod` opening pixel gate p50 0.00 / p90 0.00 / maxTile 39.00 -- identical to the
// second control -- and a same-binary order-balanced A/B/B/A on `s03`'s drain spike reading
// -9.415 / -8.170 ms/f against a control drift of 0.190. See plans/phase-G659-fix-log.md.
static bool g603SubBlendOn()
{
    static const bool on = !envFlagEnabled("DC2_G603_NO_SUBBLEND") &&
                           !envFlagDisabled("DC2_G603_SUBBLEND");
    return on;
}

// G603 (CENSUS-ONLY, always false on every shipped path). g603_admission_census.inc sets this for
// one dry-run classification so "the blend combo is the LAST blocker" is measured rather than
// inferred — see g534_admission_proof_true_but_insufficient. It is read ONLY on the blend REJECT
// arm below, i.e. on a path that is already returning false, so an accepted entry never touches it
// and the hot classifier's shape is unchanged (g602: a hot function's price can be its prologue).
static thread_local bool t_g603RelaxBlend = false;
// G604 (CENSUS-ONLY, same contract as t_g603RelaxBlend above, always false on every shipped path).
// `depth` is the FIRST failing conjunct on ~100% of the `discov` entries, and a first failing
// conjunct is not an admission proof (g534). This lets the census ask the only question a lever
// design depends on: if a discovered target were given the same non-persistent guest-depth
// contract G570/G586/G587 already hold, what would reject it NEXT? Read only on the depth
// admission arm, which every accepted `discov` entry currently never reaches.
static thread_local bool t_g604RelaxDepth = false;
static const char *const kG266FrName[] = {
    "none", "off", "cpuonlyrtt", "fbw", "mixedtgt", "classify", "mixedz", "rttdepth",
    "colorzalias", "depthsetup", "texaliasz", "submit", "zreadback"};
static std::map<uint64_t, uint64_t> g_g266CpuShapes;
static std::map<uint64_t, uint64_t> g_g266L2lShapes;
static std::map<uint64_t, uint64_t> g_g266TexShapes;
static std::atomic<uint64_t> g_g266CpuN{0}, g_g266L2lN{0}, g_g266TexN{0};

// ===== G583: WHOLE-POPULATION CPU-FALLBACK CENSUS (DEFAULT-OFF, DC2_G583_REJCENSUS=1) =====
//
// Neither existing reject census can see the population that actually drives the Ridepod boss's
// CPU band replay. G262's counters are gated `g248TargetIndex(fbp) >= 0` (the five named RTT
// targets) and G265's are gated `fbp == 0 || fbp == 0x68` (display). fbp=0x13b — G286's transient
// target, the largest single fallback source on that route — is in NEITHER set, so both censuses
// report zero for it and always did.
//
// `[G290:gpufail]` is not a substitute. It prints ENTRY 0 of the batch, which is not the entry
// that rejected: g178TryFlushGpu's classify loop stops at the FIRST failure, so entry 0 is the
// rejecter only by coincidence. Every design question an admission lever has to answer (which Z
// shape, which blend, which prim class) is a property of the REJECTER.
//
// It also ranks by ENTRIES, not events (g568_rank_fallback_by_entries_not_events). A fallback's
// cost is the entries it hands to the scalar banded replay, and entries/event spans 134..1338
// across the boss window's shapes, so an event ranking inverts the true order.
static bool g583RejCensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G583_REJCENSUS");
    return s_on;
}

// The rejecting entry's shape, stashed on exactly the same single-value contract as
// g_g266ClassifySite above (flush-owner threads only, at most one classify reject per attempt
// because the classifier returns false immediately).
static uint64_t g_g583RejEntryKey = 0ull;
static bool g_g583RejEntryValid = false;
static std::map<uint64_t, uint64_t> g_g583FallbackEvents;  // key -> events
static std::map<uint64_t, uint64_t> g_g583FallbackEntries; // key -> entries replayed
static uint64_t g_g583FallbackN = 0ull, g_g583FallbackEntriesN = 0ull;

// site/rej occupy the high bits so a sorted dump groups by reason; everything else is GS state.
static uint64_t g583ShapeKey(const G144Entry &e)
{
    const GSContext &ctx = e.ctx;
    const uint64_t ts = ctx.test;
    const uint64_t al = ctx.alpha;
    uint64_t k = 0ull;
    k |= static_cast<uint64_t>(ctx.frame.fbp & 0x1FFu);                     // 0-8
    k |= static_cast<uint64_t>(ctx.frame.fbw & 0xFu) << 9;                  // 9-12
    k |= static_cast<uint64_t>(e.prim.type & 0x7u) << 13;                   // 13-15
    k |= static_cast<uint64_t>(e.prim.tme ? 1u : 0u) << 16;                 // 16
    k |= static_cast<uint64_t>(e.prim.abe ? 1u : 0u) << 17;                 // 17
    k |= ((ts >> 16) & 1ull) << 18;                                         // 18: zte
    k |= ((ts >> 17) & 3ull) << 19;                                         // 19-20: ztst
    k |= ((ctx.zbuf >> 32) & 1ull) << 21;                                   // 21: zmsk
    k |= ((ctx.zbuf >> 24) & 0xFull) << 22;                                 // 22-25: zpsm
    k |= (ts & 1ull) << 26;                                                 // 26: ate
    k |= ((ts >> 1) & 7ull) << 27;                                          // 27-29: atst
    k |= (al & 3ull) << 30;                                                 // 30-31: A
    k |= ((al >> 2) & 3ull) << 32;                                          // 32-33: B
    k |= ((al >> 4) & 3ull) << 34;                                          // 34-35: C
    k |= ((al >> 6) & 3ull) << 36;                                          // 36-37: D
    k |= static_cast<uint64_t>(ctx.frame.psm & 0x3Fu) << 38;                // 38-43: frame psm
    k |= static_cast<uint64_t>(ctx.frame.fbmsk != 0u ? 1u : 0u) << 44;      // 44
    k |= static_cast<uint64_t>(e.prim.tme ? (ctx.tex0.psm & 0x3Fu) : 0u) << 45; // 45-50
    return k;
}

static void g583NoteRejectEntry(const G144Entry &e)
{
    if (!g583RejCensusOn())
        return;
    g_g583RejEntryKey = g583ShapeKey(e);
    g_g583RejEntryValid = true;
}

static void g583ReportFallbacks()
{
    std::fprintf(stderr, "[G583:rejcensus] fallbacks=%llu entries=%llu shapes=%zu\n",
                 static_cast<unsigned long long>(g_g583FallbackN),
                 static_cast<unsigned long long>(g_g583FallbackEntriesN),
                 g_g583FallbackEntries.size());
    for (const auto &kv : g_g583FallbackEntries)
    {
        const uint64_t k = kv.first;
        const int rej = static_cast<int>((k >> 60) & 0xFull);
        const int site = static_cast<int>((k >> 56) & 0xFull);
        const auto ev = g_g583FallbackEvents.find(k);
        std::fprintf(stderr,
                     "[G583:rejshape] entries=%llu events=%llu rej=%s site=%s fbp=%03x fbw=%u "
                     "fpsm=%x fbmskNZ=%u prim=%u tme=%u abe=%u tpsm=0x%02x abcd=%u%u%u%u "
                     "ate=%u atst=%u zte=%u ztst=%u zmsk=%u zpsm=%u\n",
                     static_cast<unsigned long long>(kv.second),
                     static_cast<unsigned long long>(ev == g_g583FallbackEvents.end()
                                                         ? 0ull : ev->second),
                     kG266FrName[rej & 0xF],
                     (rej == kG266FrClassify && site < kG262RejSiteCount && site >= 0)
                         ? kG262SiteName[site] : "-",
                     static_cast<unsigned>(k & 0x1FFull),
                     static_cast<unsigned>((k >> 9) & 0xFull),
                     static_cast<unsigned>((k >> 38) & 0x3Full),
                     static_cast<unsigned>((k >> 44) & 1ull),
                     static_cast<unsigned>((k >> 13) & 0x7ull),
                     static_cast<unsigned>((k >> 16) & 1ull),
                     static_cast<unsigned>((k >> 17) & 1ull),
                     static_cast<unsigned>((k >> 45) & 0x3Full),
                     static_cast<unsigned>((k >> 30) & 3ull),
                     static_cast<unsigned>((k >> 32) & 3ull),
                     static_cast<unsigned>((k >> 34) & 3ull),
                     static_cast<unsigned>((k >> 36) & 3ull),
                     static_cast<unsigned>((k >> 26) & 1ull),
                     static_cast<unsigned>((k >> 27) & 7ull),
                     static_cast<unsigned>((k >> 18) & 1ull),
                     static_cast<unsigned>((k >> 19) & 3ull),
                     static_cast<unsigned>((k >> 21) & 1ull),
                     static_cast<unsigned>((k >> 22) & 0xFull));
    }
}

// Called from the flush closure's CPU-fallback branch, i.e. once per g178TryFlushGpu==false.
// `entries` is the batch size that is about to be band-replayed — the ranking weight.
static void g583NoteFallback(const std::vector<G144Entry> &list)
{
    if (!g583RejCensusOn() || list.empty())
        return;
    const int rej = g_g266FlushRej & 0xF;
    const int site = (rej == kG266FrClassify) ? g_g266ClassifySite : -1;
    // Classify rejects carry the REJECTER's shape; every other reason is a whole-batch property,
    // so entry 0 is the honest representative for those.
    uint64_t k = (rej == kG266FrClassify && g_g583RejEntryValid)
                     ? g_g583RejEntryKey
                     : g583ShapeKey(list[0]);
    k |= static_cast<uint64_t>(rej & 0xF) << 60;
    k |= static_cast<uint64_t>(site & 0xF) << 56;
    g_g583RejEntryValid = false;
    ++g_g583FallbackEvents[k];
    g_g583FallbackEntries[k] += list.size();
    ++g_g583FallbackN;
    g_g583FallbackEntriesN += list.size();
    if ((g_g583FallbackN % 2048ull) == 0ull)
        g583ReportFallbacks();
}

// ===== G583: NON-PERSISTENT PER-WAVE GUEST DEPTH FOR THE 0x13b TRANSIENT TARGET =====
// G585 closes the missed G584 promotion: default-ON; rollback DC2_G583_NO_TRN13B_Z=1 or
// explicitly set DC2_G583_TRN13B_Z=0.
//
// WHAT THE CENSUS SAYS. On the Ridepod boss window (n=1441..1861) the upload-forced drain is
// 33.9 ms/f, and 62.4% of it is the CPU band replay of the 28.7% of flushes the GPU path rejects
// — not the GPU (36.8%) and not the upload edge itself. Ranked by REPLAYED ENTRIES, the single
// largest fallback source is `classify` at site `depth` on fbp=0x13b: 57.1% of every entry the
// boss window hands to the scalar replay.
//
// WHY IT REJECTS. Under universal-Z an entry that reads or writes real Z is admitted only if
// `g242GpuDepthOn() || g262Rtt || g265Display`, and `g262Rtt` names the five G248 residency slots
// plus G570's 0x13d. 0x13b is in neither list, so every depth-tested draw to it fails closed —
// including the whole Ridepod boss mesh. G573 already admitted that geometry at CAPTURE and
// bought -14.13% from the regrouping alone; its own header records that the batches then "fail
// closed" at exactly this gate and are consumed by the banded CPU replay. This is that gate.
//
// WHY IT IS SAFE TO WIDEN HERE, WHEN G569's RESIDENCY WIDENING WAS NOT. The two are different
// ownership models and only one of them can produce G569's escaped-writer defect:
//   * G569 added PERSISTENT G248 residency slots. GPU ownership of those rows survives the flush,
//     so a later guest write to a page the GPU owns is an escape — the jump/dash defect that
//     demoted it (rasterizer_tilebinning_and_probes.inc, g569RttWideMask).
//   * This rides G262/G570's NON-PERSISTENT per-wave contract: the Z window is uploaded fresh for
//     the wave and read back at the end of the same flush, and 0x13b's COLOR already returns to
//     guest VRAM in-flush under G286/G287's promoted transient contract. Nothing is added to
//     g248TargetIndex, no G261 residency record is created, and no authority survives the drain,
//     so there is no window in which an escaped writer can exist. That is the same reason G570
//     was built for 0x13d after G569's defect was found.
//
// WIDENED AS A SET, NOT AS ONE SITE (g536_invariant_untested_at_the_consuming_edge,
// g569_alias_family_admitted_whole_or_not_at_all). `g570Transient13dTarget` gates THREE conjuncts
// in this classifier — the two additive-FIX blend shapes and the guest-depth admission — and the
// G262 census found those blends on ~11% of the same RTT population that Z-tests. Widening depth
// alone would trade a `depth` reject for a `blend` reject on that slice and read as a partial
// null. All three move together or none does.
static bool g583Trn13bWaveOn()
{
    // G584 proved this path and recorded it as promoted, but its gate remained opt-in.  Keep both
    // rollback spellings so the shipped path remains bisectable without a rebuild.
    static const bool s_on = !envFlagEnabled("DC2_G583_NO_TRN13B_Z") &&
                             !envFlagDisabled("DC2_G583_TRN13B_Z");
    return s_on;
}

// The transient target this admits. Deliberately keyed on g286TransientTarget rather than on a
// bare `fbp == 0x13b` so the whole thing disarms with G286/G287's own kill switches: the
// in-flush colour readback is what makes the wave transient, and without it this would be an
// ownership claim, which is precisely what must not happen here.
static bool g583Trn13bWaveTarget(uint32_t fbp)
{
    return g583Trn13bWaveOn() && g286TransientTarget(fbp);
}

// Every site that asks "may this target take the non-persistent per-wave RTT contract?".
static inline bool g583WaveEligibleTarget(uint32_t fbp)
{
    return g248TargetIndex(fbp) >= 0 || g570Transient13dTarget(fbp) ||
           g583Trn13bWaveTarget(fbp) || g586Transient141Target(fbp) ||
           g587Transient15dTarget(fbp);
}

// Divergence proof for the paired gate. Plain counters on the flush-owner thread, same shape as
// g573NoteTri13bAdmission; DC2_G583_STAT=1 to print. Never on in a timing run.
static uint64_t g_g583Trn13bZAdmits = 0ull;
static void g583NoteTrn13bZAdmission()
{
    ++g_g583Trn13bZAdmits;
    static const bool s_stat = envFlagEnabled("DC2_G583_STAT");
    if (!s_stat)
        return;
    if (g_g583Trn13bZAdmits <= 8ull || (g_g583Trn13bZAdmits % 65536ull) == 0ull)
        std::fprintf(stderr, "[G583:trn13bz] admits=%llu\n",
                     static_cast<unsigned long long>(g_g583Trn13bZAdmits));
}

static void g262NoteSeen(const G144Entry &e)
{
    if (!g262CensusOn() || g248TargetIndex(e.ctx.frame.fbp) < 0)
        return;
    g_g262RttSeen.fetch_add(1u, std::memory_order_relaxed);
}

static void g262NoteReject(int site, const G144Entry &e)
{
    // G266: remember the site unconditionally (single int, flush-owner threads only) so the
    // consumer census can attribute a classify-driven CPU replay to its exact reject site.
    g_g266ClassifySite = site;
    g265NoteReject(site, e);
    if (!g262CensusOn() || g248TargetIndex(e.ctx.frame.fbp) < 0)
        return;
    const uint64_t n = g_g262Rej[site].fetch_add(1u, std::memory_order_relaxed) + 1u;
    const GSContext &ctx = e.ctx;
    const uint64_t ts = ctx.test;
    const uint64_t al = ctx.alpha;
    uint64_t k = 0;
    k |= ((ctx.zbuf >> 32) & 1ull);                              // 0: zmsk
    k |= ((ts >> 16) & 1ull) << 1;                               // 1: zte
    k |= ((ts >> 17) & 3ull) << 2;                               // 2-3: ztst
    k |= ((ctx.zbuf >> 24) & 0xFull) << 4;                       // 4-7: zpsm
    k |= (e.prim.tme ? (static_cast<uint64_t>(ctx.tex0.psm) & 0x3Full) : 0x3Full) << 8; // 8-13
    k |= (static_cast<uint64_t>(ctx.clamp) & 3ull) << 14;        // 14-15: wrapU
    k |= ((static_cast<uint64_t>(ctx.clamp) >> 2) & 3ull) << 16; // 16-17: wrapV
    k |= (ts & 1ull) << 18;                                      // 18: ate
    k |= ((ts >> 1) & 7ull) << 19;                               // 19-21: atst
    k |= ((ts >> 4) & 0xFFull) << 22;                            // 22-29: aref
    k |= ((ts >> 12) & 3ull) << 30;                              // 30-31: afail
    k |= ((ts >> 14) & 1ull) << 32;                              // 32: date
    k |= (e.prim.tme ? 1ull : 0ull) << 33;                       // 33: tme
    k |= (e.prim.abe ? 1ull : 0ull) << 34;                       // 34: abe
    k |= (al & 3ull) << 35;                                      // 35-36: a
    k |= ((al >> 2) & 3ull) << 37;                               // 37-38: b
    k |= ((al >> 4) & 3ull) << 39;                               // 39-40: c
    k |= ((al >> 6) & 3ull) << 41;                               // 41-42: d
    k |= ((al >> 32) & 0xFFull) << 43;                           // 43-50: fix
    k |= ((ctx.frame.fbmsk != 0u) ? 1ull : 0ull) << 51;          // 51
    k |= ((ctx.fba & 1ull) != 0ull ? 1ull : 0ull) << 52;         // 52
    k |= (e.pabe ? 1ull : 0ull) << 53;                           // 53
    k |= (static_cast<uint64_t>(e.prim.type) & 0x7ull) << 54;    // 54-56
    k |= (static_cast<uint64_t>(g248TargetIndex(ctx.frame.fbp)) & 0x7ull) << 57; // 57-59
    k |= (static_cast<uint64_t>(site) & 0x7ull) << 60;           // 60-62
    ++g_g262Shapes[k];
    if ((n & 0x7FFull) == 0ull) // periodic report every 2048 RTT rejects at this site
        g262Report();
}

bool g178ClassifyEntry(const G144Entry &e, G178EntryState &st)
{
    g265NoteSeen(e);
    g262NoteSeen(e);
    const GSContext &ctx = e.ctx;
    // G364: GS per-pixel fog is exact on the GPU (interpolated F, RGB lerp towards FOGCOL before
    // blending), so a fogged entry stays GPU-eligible rather than falling back to CPU.
    st.fge = (e.prim.fge && g364FogEnabled()) ? 1u : 0u;
    st.fogCol = st.fge ? e.fogcol : 0u;
    // Frame target: CT32 display buffer, no channel masking, no dest-alpha tricks.
    if (ctx.frame.psm != GS_PSM_CT32 || ctx.frame.fbmsk != 0u || ctx.frame.fbw == 0u ||
        (ctx.fba & 1ull) != 0ull || e.pabe)
    {
        g262NoteReject(kG262RejFrame, e);
        g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const uint64_t ts = ctx.test;
    const uint32_t ate = static_cast<uint32_t>(ts & 1u);
    const uint32_t atst = static_cast<uint32_t>((ts >> 1) & 7u);
    const uint32_t aref = static_cast<uint32_t>((ts >> 4) & 0xffu);
    const uint32_t afail = static_cast<uint32_t>((ts >> 12) & 3u);
    const uint32_t date = static_cast<uint32_t>((ts >> 14) & 1u);
    // Alpha test is normally required to be a structural no-op. G265 admits only the two
    // MAP-0 display shapes proven by census: GEQUAL AREF={1,64,127}, AFAIL=KEEP. Shader discard is
    // exact for KEEP because it suppresses framebuffer and depth writes together; every other
    // ATST/AFAIL/DATE combination retains the CPU fallback (G240's independent FB/Z contract).
    uint8_t g265AlphaMode = 0u;
    const bool alphaNoOp = ate == 0u || atst == 1u || (atst == 5u && aref == 0u);
    if (date != 0u || !alphaNoOp)
    {
        const bool g265Keep = date == 0u && g265DisplayZWaveOn() &&
                              g265DisplayTarget(ctx.frame.fbp) && ate != 0u && atst == 5u &&
                              (aref == 1u || aref == 64u || aref == 127u) && afail == 0u;
        if (!g265Keep)
        {
            g262NoteReject(kG262RejATest, e);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const uint8_t refCode = aref == 1u ? 0u : (aref == 64u ? 1u : 2u);
        g265AlphaMode = static_cast<uint8_t>(0x20u | (refCode << 6u));
    }
    // Scissor (inclusive) sanity; FBO rows capped at 512.
    if (ctx.scissor.x1 < ctx.scissor.x0 || ctx.scissor.y1 < ctx.scissor.y0 ||
        ctx.scissor.y1 >= 512u || ctx.scissor.x1 >= 1024u)
    {
        g262NoteReject(kG262RejScissor, e);
        g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    st.scX0 = ctx.scissor.x0; st.scY0 = ctx.scissor.y0;
    st.scX1 = ctx.scissor.x1; st.scY1 = ctx.scissor.y1;

    // Blend classification (writePixel's ((A-B)*C>>7)+D with the census-confirmed combos).
    if (!e.prim.abe)
        st.blend = 0;
    else
    {
        const uint64_t al = ctx.alpha;
        const uint32_t a = static_cast<uint32_t>(al & 3u), b = static_cast<uint32_t>((al >> 2) & 3u);
        const uint32_t c = static_cast<uint32_t>((al >> 4) & 3u), d = static_cast<uint32_t>((al >> 6) & 3u);
        if (a == b && d == 0u)
            st.blend = 0; // (X-X)*C + Cs == Cs
        else if (a == 0u && b == 1u && c == 0u && d == 1u)
            st.blend = 1; // (Cs-Cd)*As + Cd
        else if (a == 0u && b == 2u && c == 0u && d == 1u)
            st.blend = 2; // Cs*As + Cd
        else if (a == 0u && b == 2u && c == 2u && d == 1u && ((al >> 32) & 0xFFull) == 128ull &&
                 g262WideOn() && g583WaveEligibleTarget(ctx.frame.fbp))
            st.blend = 3; // (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd (G262 census shape, RTT waves only)
        else if (a == 2u && b == 0u && c == 2u && d == 1u && ((al >> 32) & 0xFFull) == 128ull &&
                 g262WideOn() && g583WaveEligibleTarget(ctx.frame.fbp))
            st.blend = 4; // (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs (G262 census shape, RTT waves only)
        else if (a == 2u && b == 1u && c == 0u && d == 1u &&
                 g265DisplayZWaveOn() && g662DestBlendTarget(ctx.frame.fbp))
            st.blend = 5; // G265 census: (0-Cd)*As>>7 + Cd == Cd*(1-As)
        else if (a == 2u && b == 0u && c == 0u && d == 1u && g603SubBlendOn() &&
                 g265DisplayZWaveOn() && g662DestBlendTarget(ctx.frame.fbp))
            st.blend = 6; // G603 census: (0-Cs)*As>>7 + Cd == Cd - Cs*As/128 (see the header above)
        else if (t_g603RelaxBlend)
        {
            st.blend = 0; // G603 census pass 2 only — never reachable on a shipped path.
        }
        else
        {
            g262NoteReject(kG262RejBlend, e);
            g_g178RejectBlend.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    }

    // Texture state.
    if (e.prim.tme)
    {
        const uint8_t psm = ctx.tex0.psm;
        const bool okPsm = psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 ||
                           psm == GS_PSM_CT16S || psm == GS_PSM_T8 || psm == GS_PSM_T4 ||
                           psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
        const uint8_t wu = static_cast<uint8_t>(ctx.clamp & 3u);
        const uint8_t wv = static_cast<uint8_t>((ctx.clamp >> 2) & 3u);
        st.texW = 1 << ctx.tex0.tw;
        st.texH = 1 << ctx.tex0.th;
        if (!okPsm || ctx.tex0.tbw == 0u || wu > 1u || wv > 1u ||
            st.texW > 1024 || st.texH > 1024)
        {
            g262NoteReject(kG262RejTex, e);
            g_g178RejectTex.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        st.texKey = g178TexKey(ctx, e.texa, e.texclut);
        st.tfx = ctx.tex0.tfx;
        st.tcc = ctx.tex0.tcc;
        // G255: CPU drawSprite selects linear filtering from MMAG OR the MMIN linear/mipmap
        // classes. Looking at MMAG alone made the T8 RTT downsample/effect sprites point-sampled
        // on GPU while their proven CPU replay was bilinear.
        const bool g286ExactEntry = g286TransientTarget(ctx.frame.fbp);
        const bool g570ExactEntry = g570Transient13dTarget(ctx.frame.fbp);
        const bool g586ExactEntry = g586Transient141Target(ctx.frame.fbp);
        const bool g587ExactEntry = g587Transient15dTarget(ctx.frame.fbp);
        const bool g255RttEntry =
            (g255GpuRttOn() && g248TargetIndex(ctx.frame.fbp) >= 0) ||
            g286ExactEntry || g570ExactEntry || g586ExactEntry || g587ExactEntry;
        st.bilinear = g255RttEntry
            ? tex1UsesLinearFilter(ctx.tex1)
            : (((ctx.tex1 >> 5u) & 1u) != 0u);
        // G179: GL_REPEAT wraps the final NORMALIZED [0,1] UV -- under GL_NEAREST, a sample that
        // floating-point-rounds to land exactly on (or a hair past) the wrap seam reads the
        // texture's OPPOSITE edge instead of clamping, bleeding a stray row/column of an unrelated
        // atlas cell into the draw (found on the debug-menu PSMT4HH font, which is REPEAT-wrapped
        // per G5, and is always point-sampled per G8 -- DC2's 2D UI/text is never bilinear). GS
        // hardware itself only cares about wrap for genuine out-of-declared-range addressing, which
        // point-sampled UI glyphs never actually need (each glyph's own UV stays within one atlas
        // cell) -- CLAMP_TO_EDGE removes the seam-bleed risk with no behavior change for in-range
        // sampling. Bilinear-sampled REPEAT textures (tiled background art) are unaffected.
        // G256's explicit texel fetch applies the CPU sampler's wrap mode itself, including point
        // samples.  Preserve G179's clamp-at-the-normalized-seam convention for the legacy shader.
        const bool exactSampler = (g256ExactRttOn() && g255RttEntry) ||
                                  g286ExactEntry || g570ExactEntry || g586ExactEntry ||
                                  g587ExactEntry;
        st.wrapU = exactSampler ? wu : (st.bilinear ? wu : 1u);
        st.wrapV = exactSampler ? wv : (st.bilinear ? wv : 1u);
        if (g_g633AttachTextureHook != nullptr) g_g633AttachTextureHook(e, st);
    }

    // G270 line entries deliberately retain drawLine's semantics: no texture and no depth
    // read/write. Alpha test, blend, scissor, and framebuffer validation above still apply. A
    // defensive gate keeps an accidentally captured textured/off-target line on CPU fallback.
    if (g270LineType(static_cast<uint32_t>(e.prim.type)))
    {
        // G595: read the ARM-AWARE accessor, so a line captured under the paired A/B's candidate
        // arm is classified under the same arm. The arm is frame-stable (g419AbTick, hold 16) and
        // every batch drains inside the frame that captured it, so the two can never disagree.
        if (!g270LineWaveActive() || e.prim.tme ||
            (ctx.frame.fbp != 0x0u && ctx.frame.fbp != 0x68u))
        {
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        st.depthFunc = g265AlphaMode;
        st.depthWrite = false;
        return true;
    }

    // Depth: the CPU rasterizer's SHIPPED title model — Z applies only to title-rock-scope tris
    // (private per-fbp Z, g106TitleZScope); sprites never touch Z (drawSprite has no Z code).
    st.depthFunc = g265AlphaMode;
    st.depthWrite = false;
    static const bool s_zTestDisabled = (std::getenv("DC2_NO_ZTEST") != nullptr);
    const uint32_t zte = static_cast<uint32_t>((ts >> 16) & 1u);
    const uint32_t ztst = static_cast<uint32_t>((ts >> 17) & 3u);
    const bool zmsk = ((ctx.zbuf >> 32) & 1ull) != 0ull;
    if (g203UniversalZEnabled())
    {
        // G203 correctness-first: under universal-Z the CPU VRAM Z buffer is authoritative for ALL
        // targets and prims (incl. the game's clear-sprite). The G178 GPU path reads back color
        // only, not the live VRAM Z page, so any entry that reads OR writes real Z must run on the
        // CPU replay to stay coherent. Trivial ztst=ALWAYS + zmsk (no test, no write) draws remain
        // GPU-eligible (e.g. UI/HUD sprites). GPU depth parity for universal-Z is a G204 follow-up.
        const bool zWrites = (zte != 0u) && !zmsk && (ztst != 0u);
        const bool zTests  = (zte != 0u) && (ztst != 1u); // NEVER/GEQUAL/GREATER all need Z
        if (!s_zTestDisabled && (zWrites || zTests))
        {
            // G404: ZBP 0xd0 is one physical depth surface across display and RTT color targets.
            // Its guest clear is mirrored outside aliased VRAM, so every user of that mirror must
            // remain on CPU replay; an RTT GPU wave would upload stale physical page contents.
            const uint32_t g403Zbp = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
            const uint32_t g403Zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
            if (g404SharedZScope(ctx.frame.fbp, ctx.frame.fbw, g403Zbp, g403Zpsm))
            {
                // G411: G405's private mirror is now an explicit native-depth owner. The backend
                // already keys depth by (ZBP, ZPSM, FBW), independent of color FBP, so display
                // clears and RTT draws can share the same GPU attachment without touching the
                // aliased physical Z page. Control remains whole-flush CPU replay.
                if (!g411SharedGpuDepthOn())
                {
                    g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
            }
            // G262: widened RTT-family entries take the same guestDepth derivation as G242, but
            // the flush executes it as a NON-PERSISTENT per-wave Z round-trip (no ownership).
            // G570's 0x13d path uses this same non-persistent guest-depth attachment, but keeps
            // COLOR transient too: both authorities return to CPU/VRAM before the flush ends.
            // G583 adds G286's 0x13b on that same transient-colour contract — see
            // g583Trn13bWaveOn above for why this cannot reproduce G569's escaped writer.
            // G604: the discovered targets join this admission and NOTHING else. They are
            // deliberately kept out of g583WaveEligibleTarget, whose other two uses select the
            // FIX=128 blend modes 3/4 — widening those would be a second, unmeasured change
            // riding on this one (g534: an admission proof answers ONE question).
            const bool g262Rtt = g262WideOn() && (g583WaveEligibleTarget(ctx.frame.fbp) ||
                                                  g604DiscovTransientTarget(ctx.frame.fbp));
            const bool g265Display = g265DisplayZWaveOn() && g265DisplayTarget(ctx.frame.fbp);
            // t_g604RelaxDepth is the census-only arm (see its declaration): it grants this entry
            // the guest-depth contract it does not yet have, purely so the dry run can report the
            // NEXT rejecting conjunct instead of stopping at this one.
            if (g242GpuDepthOn() || g262Rtt || g265Display || t_g604RelaxDepth)
            {
                const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
                const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
                // DC2's validated 3D paths all use PSMZ24. Other formats remain on the proven CPU
                // replay until they have their own exact float/packing verification.
                if (zpsm != 0x1u)
                {
                    g262NoteReject(kG262RejZPsm, e);
                    g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                if (ztst == 0u)
                {
                    st.skip = true; // NEVER neither draws nor writes Z.
                    return true;
                }
                st.guestDepth = true;
                st.zbpPage = static_cast<uint16_t>(zbpPage);
                st.zpsm = static_cast<uint8_t>(zpsm);
                // High bit 0x10 is private to G242: clamp interpolated fragment Z to uint24 in
                // the shader before applying the existing ALWAYS/GEQUAL/GREATER mapping.
                st.depthFunc = static_cast<uint8_t>(g265AlphaMode | 0x10u |
                    ((ztst == 1u) ? 1u : (ztst == 2u ? 2u : 3u)));
                st.depthWrite = !zmsk;
                // G583 PATH COUNTER. A null paired A/B is only evidence if something proves the
                // arms actually diverged (g538_null_ab_needs_a_path_counter). This entry reached
                // guest-depth admission ONLY because of this phase's widening, so it counts
                // exactly the entries the lever moved off the scalar band replay.
                if (g583Trn13bWaveTarget(ctx.frame.fbp))
                    g583NoteTrn13bZAdmission();
                else if (g586Transient141Target(ctx.frame.fbp))
                    g586NoteTransient141Admission(static_cast<uint32_t>(e.prim.type));
                else if (g587Transient15dTarget(ctx.frame.fbp))
                    g587NoteTransient15dAdmission(static_cast<uint32_t>(e.prim.type));
                else if (g604DiscovTransientTarget(ctx.frame.fbp))
                    g604NoteDiscovAdmission(static_cast<uint32_t>(e.prim.type)); // g538 path counter
                return true;
            }
            g262NoteReject(kG262RejDepth, e);
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    // Legacy (DC2_G203_LEGACY_Z=1): the old per-scope GPU-depth model.
    if (e.prim.type != GS_PRIM_SPRITE)
    {
        const bool rockScope = e.titleRockScope && e.prim.tme && ctx.frame.fbp != 0x139u &&
                               ctx.tex0.tbp0 >= 0x2720u && ctx.tex0.tbp0 <= 0x3960u;
        const uint32_t zbpPage = static_cast<uint32_t>(ctx.zbuf & 0x1FFu);
        const uint32_t zpsm = static_cast<uint32_t>((ctx.zbuf >> 24) & 0xFu);
        if (zte != 0u && g202TownZScope(ctx.frame.fbp, zbpPage, zpsm))
        {
            g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
            return false; // G202 town Z needs real VRAM Z read/write; GPU path readbacks color only.
        }
        const bool zEnabled = !s_zTestDisabled && g106TitleZEnabled() && rockScope && zte != 0u;
        if (zEnabled)
        {
            if (zpsm != 0u)
            {
                g_g178RejectState.fetch_add(1u, std::memory_order_relaxed);
                return false; // title is PSMZ32; other Z formats untested on this path
            }
            if (ztst == 0u)
            {
                st.skip = true; // NEVER: draws nothing
                return true;
            }
            st.depthFunc = static_cast<uint8_t>(g265AlphaMode |
                ((ztst == 1u) ? 1u : (ztst == 2u ? 2u : 3u)));
            st.depthWrite = !zmsk;
        }
    }
    return true;
}

bool g178StateEqual(const G178EntryState &x, const G178EntryState &y)
{
    return x.texKey == y.texKey && x.blend == y.blend && x.tfx == y.tfx && x.tcc == y.tcc &&
           x.fge == y.fge && x.fogCol == y.fogCol && // G364

           x.depthFunc == y.depthFunc && x.depthWrite == y.depthWrite &&
           x.bilinear == y.bilinear && x.wrapU == y.wrapU && x.wrapV == y.wrapV &&
           x.scX0 == y.scX0 && x.scY0 == y.scY0 && x.scX1 == y.scX1 && x.scY1 == y.scY1;
}

void g178PushVertex(std::vector<G178Vtx> &out, const GSVertex &v, int ofx, int ofy,
                    const GSVertex &colorSrc, bool tme, bool fst, int texW, int texH,
                    float uvShiftU, float uvShiftV)
{
    G178Vtx o;
    o.x = v.x - static_cast<float>(ofx);
    o.y = v.y - static_cast<float>(ofy);
    o.z = v.z;
    o.r = colorSrc.r; o.g = colorSrc.g; o.b = colorSrc.b; o.a = colorSrc.a;
    // G364: F rides the GEOMETRIC vertex (a real GS interpolator, independent of IIP). Sprite
    // callers pre-copy the flat second-vertex F onto their corner vertices, mirroring `z`.
    o.fog = v.fog; o.pad0 = 0; o.pad1 = 0; o.pad2 = 0;
    o.drawState0 = 0.0f; o.drawState1 = 0.0f;
    o.drawState2 = 0.0f; o.drawState3 = 0.0f;
    if (!tme)
    {
        o.sq = 0.0f; o.tq = 0.0f; o.iq = 1.0f;
    }
    else if (fst)
    {
        o.sq = (static_cast<float>(v.u) / 16.0f + uvShiftU) / static_cast<float>(texW);
        o.tq = (static_cast<float>(v.v) / 16.0f + uvShiftV) / static_cast<float>(texH);
        o.iq = 1.0f;
    }
    else
    {
        // G222 CPU parity (drawTriangle STQ path): raw S/T/Q per vertex; the FS divides the
        // noperspective-lerped .xy by the lerped .z — the GS's own hyperbolic interpolation,
        // matching the G222 CPU sampler (u = lerp(S)/lerp(Q)). DC2_G222_AFFINE_TEX=1 restores
        // the pre-G222 affine s/|q| feed (parity with the old CPU sampler).
        static const bool s_g222Affine = envFlagEnabled("DC2_G222_AFFINE_TEX");
        if (s_g222Affine)
        {
            const float invQ = 1.0f / fabsQ(v.q);
            o.sq = v.s * invQ;
            o.tq = v.t * invQ;
            o.iq = 1.0f;
        }
        else
        {
            o.sq = v.s;
            o.tq = v.t;
            o.iq = fabsQ(v.q); // zero-guard only (returns q signed); FS max() guards the divide
        }
    }
    out.push_back(o);
}

// G270: turn the exact CPU Bresenham result into GL triangles. Each visited integer pixel becomes
// one [x,x+1]x[y,y+1] quad, so the native backend's existing triangle path preserves scissor,
// alpha-test, blend, target ownership, and submission order without relying on host line rules.
void g270AppendLinePixels(std::vector<G178Vtx> &out, const G144Entry &e)
{
    const GSVertex &v0 = e.v[0];
    const GSVertex &v1 = e.v[1];
    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    const int x1 = static_cast<int>(v1.x) - ofx;
    const int y1 = static_cast<int>(v1.y) - ofy;
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        const float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        GSVertex color = v1;
        if (e.prim.iip)
        {
            color.r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            color.g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            color.b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            color.a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }

        GSVertex c00 = color, c10 = color, c11 = color, c01 = color;
        const float rawX = static_cast<float>(x0 + ofx);
        const float rawY = static_cast<float>(y0 + ofy);
        c00.x = rawX;        c00.y = rawY;
        c10.x = rawX + 1.0f; c10.y = rawY;
        c11.x = rawX + 1.0f; c11.y = rawY + 1.0f;
        c01.x = rawX;        c01.y = rawY + 1.0f;
        c00.z = c10.z = c11.z = c01.z = v1.z;
        g178PushVertex(out, c00, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c10, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c11, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c00, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c11, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);
        g178PushVertex(out, c01, ofx, ofy, color, false, false, 1, 1, 0.0f, 0.0f);

        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        ++step;
    }
}
} // namespace

// G242 private side channel implemented in ps2_gs_gpu_raster.cpp. Kept out of headers so this
// phase only rebuilds the two runtime translation units.
bool g242_backend_submit_depth(G178Batch &batch, uint64_t depthKey,
                               const std::vector<float> *depthUpload,
                               int depthY, int depthRows);
bool g275_backend_submit_depth_readback(G178Batch &batch, uint64_t depthKey,
                                        const std::vector<float> *depthUpload,
                                        int depthY, int depthRows,
                                        int readY, int readRows,
                                        std::vector<float> &depthReadback);
bool g242_backend_read_depth(uint64_t depthKey, int width, int height,
                             int depthY, int depthRows, std::vector<float> &depthReadback);
// G305: one-slot move-owned async submit bridges (ps2_gs_gpu_raster.cpp), default-off.
bool g178_backend_submit_async(G178Batch &batch);
bool g178_backend_drain_async(bool &ok);
// G415: publish the exact GL row window the front-end will consume out of the NEXT synchronous
// submit's colour readback (ps2_gs_gpu_raster.cpp). Consumed-and-cleared by that submit.
void g415_backend_set_color_window(int glY, int rows);
namespace
{
struct G242DepthState
{
    uint64_t key = 0u;
    uint32_t zbpPage = 0u;
    uint32_t zpsm = 0u;
    uint32_t fbw = 0u;
    int fbW = 0;
    bool valid = false;
    bool dirty = false;
    int dirtyLo = 512;
    int dirtyHi = -1;
    uint64_t guestGen = 0u;
};

std::unordered_map<uint64_t, G242DepthState> g_g242DepthStates;
std::atomic<uint64_t> g_g242DepthUploads{0};
std::atomic<uint64_t> g_g242DepthSyncs{0};
std::atomic<uint64_t> g_g242InvariantFails{0};

uint64_t g242DepthKey(uint32_t zbpPage, uint32_t zpsm, uint32_t fbw)
{
    return (1ull << 63u) | static_cast<uint64_t>(zbpPage & 0x1FFu) |
           (static_cast<uint64_t>(zpsm & 0xFu) << 9u) |
           (static_cast<uint64_t>(fbw & 0x3Fu) << 16u);
}

uint64_t g242GuestGen(const G242DepthState &s)
{
    return g178GenSumRect(s.zbpPage << 5u, s.fbw, GS_PSM_Z24, 0u, 0u,
                          static_cast<uint32_t>(s.fbW), 512u);
}

bool g242SyncDepthState(uint8_t *vram, G242DepthState &s)
{
    if (!s.valid || !s.dirty)
        return true;
    if (vram == nullptr || s.dirtyHi < s.dirtyLo)
        return false;
    const int syncLo = s.dirtyLo;
    const int syncHi = s.dirtyHi;
    const int rows = syncHi - syncLo + 1;
    const int glY = 512 - 1 - syncHi;
    static std::vector<float> readback;
    readback.clear();
    if (!g242_backend_read_depth(s.key, s.fbW, 512, glY, rows, readback) ||
        readback.size() != static_cast<size_t>(s.fbW) * rows)
    {
        std::fprintf(stderr, "[G242:sync] FAIL key=0x%llx rows=%d..%d\n",
                     static_cast<unsigned long long>(s.key), s.dirtyLo, s.dirtyHi);
        return false;
    }
    constexpr double kDepthToZ = 4294967296.0;
    const uint32_t zbpBlock = s.zbpPage << 5u;
    for (int y = s.dirtyLo; y <= s.dirtyHi; ++y)
    {
        const size_t srcRow = static_cast<size_t>(s.dirtyHi - y) * s.fbW;
        for (int x = 0; x < s.fbW; ++x)
        {
            double scaled = static_cast<double>(readback[srcRow + x]) * kDepthToZ;
            if (scaled < 0.0) scaled = 0.0;
            if (scaled > 16777215.0) scaled = 16777215.0;
            GSMem::WriteZ24(vram, zbpBlock, s.fbw, static_cast<uint32_t>(x),
                            static_cast<uint32_t>(y), static_cast<uint32_t>(scaled));
        }
    }
    g178BumpRectImpl(zbpBlock, s.fbw, GS_PSM_Z24, 0u,
                     static_cast<uint32_t>(s.dirtyLo), static_cast<uint32_t>(s.fbW),
                     static_cast<uint32_t>(rows),
                     22u); // G582 source 22: RTT wave depth publication
    // Decoded CPU textures use their own coarse VRAM generation.  A Z page may also be texture or
    // CLUT storage, so publishing GPU depth bytes must invalidate that cache as well.
    g149BumpVramGen();
    s.guestGen = g242GuestGen(s);
    s.dirty = false;
    s.dirtyLo = 512;
    s.dirtyHi = -1;
    const uint64_t syncNo = g_g242DepthSyncs.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g242StatEnabled() && syncNo <= 32u)
        std::fprintf(stderr, "[G242:sync] n=%llu key=0x%llx zbp=0x%x fbw=%u rows=%d..%d\n",
                     static_cast<unsigned long long>(syncNo),
                     static_cast<unsigned long long>(s.key), s.zbpPage, s.fbw,
                     syncLo, syncHi);
    return true;
}

bool g242SyncAllDepth(uint8_t *vram)
{
    if (!g242GpuDepthOn())
        return true;
    bool ok = true;
    for (auto &kv : g_g242DepthStates)
        ok = g242SyncDepthState(vram, kv.second) && ok;
    return ok;
}

void g242MarkRectPages(std::array<uint8_t, kG178PageCount> &pages,
                       uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u)
        return;
    // G471: shift form of the same page-dimension divides (see g178PageShifts).
    uint32_t pwSh = 0u, phSh = 0u;
    g178PageShifts(psm, pwSh, phSh);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) << 6u) >> pwSh);
    const uint32_t basePage = bpBlocks >> 5u;
    const uint32_t px0 = x >> pwSh, px1 = (x + w - 1u) >> pwSh;
    const uint32_t py0 = y >> phSh, py1 = (y + h - 1u) >> phSh;
    for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
        for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            pages[pg & (kG178PageCount - 1u)] = 1u;
            pages[(pg + 1u) & (kG178PageCount - 1u)] = 1u;
        }
}

// ===== G471: page-set overlap without materializing two 512-byte maps =====
//
// g242RectsOverlap used to zero TWO std::array<uint8_t,512>, mark both rects into them, then scan
// all 512 entries. That is ~1 KB of memset plus a 512-entry scan PER CALL, and the call runs once
// or twice per captured draw through g242WriteOverlapsDirtyDepth (~20k/f on MAP-0). G471's PC
// sampler put the g178NoteVramWriteRect family at ~3.8% of the GS worker's whole span with `memset`
// visible beside it.
//
// The replacement enumerates exactly the same pages (same walk, same +1 slack, same 512-page wrap)
// but stores side A as a 512-BIT set — 64 bytes, one cache line, zeroed with 8 stores — and then
// TESTS side B's pages against it, returning at the first hit instead of scanning a full map.
// Set-intersection semantics are unchanged: a non-empty intersection exists iff some page of B is
// marked in A. Oracle: DC2_G471_PAGESHIFT_VERIFY=1 re-runs the original two-array form and reports
// any disagreement.
using G471PageBits = uint64_t[kG178PageCount / 64];

static inline void g471ClearPageBits(uint64_t *bits)
{
    for (uint32_t i = 0; i < kG178PageCount / 64u; ++i)
        bits[i] = 0ull;
}

static void g471MarkRectBits(uint64_t *bits, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u)
        return;
    uint32_t pwSh = 0u, phSh = 0u;
    g178PageShifts(psm, pwSh, phSh);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) << 6u) >> pwSh);
    const uint32_t basePage = bpBlocks >> 5u;
    const uint32_t px0 = x >> pwSh, px1 = (x + w - 1u) >> pwSh;
    const uint32_t py0 = y >> phSh, py1 = (y + h - 1u) >> phSh;
    const auto set = [&](uint32_t pg) {
        const uint32_t i = pg & (kG178PageCount - 1u);
        bits[i >> 6u] |= (1ull << (i & 63u));
    };
    for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
        for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            set(pg);
            set(pg + 1u);
        }
}

// True as soon as any page of this rect is marked in `bits` (same enumeration as g471MarkRectBits).
static bool g471RectHitsBits(const uint64_t *bits, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u)
        return false;
    uint32_t pwSh = 0u, phSh = 0u;
    g178PageShifts(psm, pwSh, phSh);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) << 6u) >> pwSh);
    const uint32_t basePage = bpBlocks >> 5u;
    const uint32_t px0 = x >> pwSh, px1 = (x + w - 1u) >> pwSh;
    const uint32_t py0 = y >> phSh, py1 = (y + h - 1u) >> phSh;
    const auto test = [&](uint32_t pg) {
        const uint32_t i = pg & (kG178PageCount - 1u);
        return (bits[i >> 6u] & (1ull << (i & 63u))) != 0ull;
    };
    for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
        for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            if (test(pg) || test(pg + 1u))
                return true;
        }
    return false;
}

// Original two-array form, retained ONLY as the bring-up oracle for the bitset path.
static bool g471RectsOverlapRef(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
                                uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                                uint32_t bBp, uint32_t bBw, uint32_t bPsm,
                                uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    std::array<uint8_t, kG178PageCount> a{};
    std::array<uint8_t, kG178PageCount> b{};
    g242MarkRectPages(a, aBp, aBw, aPsm, ax, ay, aw, ah);
    g242MarkRectPages(b, bBp, bBw, bPsm, bx, by, bw, bh);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0u && b[i] != 0u)
            return true;
    return false;
}

bool g242RectsOverlap(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
                      uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                      uint32_t bBp, uint32_t bBw, uint32_t bPsm,
                      uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    G471PageBits bits;
    g471ClearPageBits(bits);
    g471MarkRectBits(bits, aBp, aBw, aPsm, ax, ay, aw, ah);
    const bool hit = g471RectHitsBits(bits, bBp, bBw, bPsm, bx, by, bw, bh);
    if (g471PageShiftVerifyOn())
    {
        const bool ref = g471RectsOverlapRef(aBp, aBw, aPsm, ax, ay, aw, ah,
                                             bBp, bBw, bPsm, bx, by, bw, bh);
        if (ref != hit)
        {
            static std::atomic<uint32_t> s_bad{0};
            if (s_bad.fetch_add(1u, std::memory_order_relaxed) < 32u)
                std::fprintf(stderr,
                             "[G471:overlap-BAD] a=(bp=0x%x bw=%u psm=0x%x %u,%u %ux%u) "
                             "b=(bp=0x%x bw=%u psm=0x%x %u,%u %ux%u) bits=%d ref=%d\n",
                             aBp, aBw, aPsm, ax, ay, aw, ah,
                             bBp, bBw, bPsm, bx, by, bw, bh, hit ? 1 : 0, ref ? 1 : 0);
        }
    }
    return hit;
}

// G273: exact page-set overlap for PAGE-ALIGNED bases. g242MarkRectPages deliberately marks pg+1
// as slack for non-page-aligned block bases, but applying that slack unconditionally turns the
// measured MAP-0 pair into a false alias: fbp=0x68 color rows use pages 104..207 and ZBP=0xd0
// starts at page 208. Separate GL color/depth attachments are valid for these disjoint pages.
// Unknown/non-aligned layouts never call this helper and retain the conservative G242 answer.
bool g273AlignedRectsOverlapExact(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
                                  uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                                  uint32_t bBp, uint32_t bBw, uint32_t bPsm,
                                  uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    if ((aBp & 31u) != 0u || (bBp & 31u) != 0u || aw == 0u || ah == 0u || bw == 0u || bh == 0u)
        return true; // fail closed; caller should normally have declined this helper
    auto markExact = [](std::array<uint8_t, kG178PageCount> &pages,
                        uint32_t bp, uint32_t width64, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        uint32_t pw = 0u, ph = 0u;
        g178PageDims(psm, pw, ph);
        const uint32_t widthPages = std::max(1u, (std::max(1u, width64) * 64u) / pw);
        const uint32_t basePage = bp >> 5u;
        const uint32_t px0 = x / pw, px1 = (x + w - 1u) / pw;
        const uint32_t py0 = y / ph, py1 = (y + h - 1u) / ph;
        for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
            for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
                pages[(basePage + py * widthPages + px) & (kG178PageCount - 1u)] = 1u;
    };
    std::array<uint8_t, kG178PageCount> a{};
    std::array<uint8_t, kG178PageCount> b{};
    markExact(a, aBp, aBw, aPsm, ax, ay, aw, ah);
    markExact(b, bBp, bBw, bPsm, bx, by, bw, bh);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0u && b[i] != 0u)
            return true;
    return false;
}

bool g242StateOverlapsRect(const G242DepthState &s,
                           uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t depthY = s.dirty && s.dirtyHi >= s.dirtyLo
                                ? static_cast<uint32_t>(s.dirtyLo)
                                : 0u;
    const uint32_t depthH = s.dirty && s.dirtyHi >= s.dirtyLo
                                ? static_cast<uint32_t>(s.dirtyHi - s.dirtyLo + 1)
                                : 512u;
    return g242RectsOverlap(s.zbpPage << 5u, s.fbw, GS_PSM_Z24,
                            0u, depthY, static_cast<uint32_t>(s.fbW), depthH,
                            bpBlocks, bw64, psm, x, y, w, h);
}

bool g242SyncOverlappingDepth(uint8_t *vram,
                              uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint64_t ignoreKey = 0u)
{
    if (!g242GpuDepthOn())
        return true;
    bool ok = true;
    for (auto &kv : g_g242DepthStates)
    {
        G242DepthState &s = kv.second;
        if (s.key == ignoreKey || !s.dirty ||
            !g242StateOverlapsRect(s, bpBlocks, bw64, psm, x, y, w, h))
            continue;
        ok = g242SyncDepthState(vram, s) && ok;
    }
    return ok;
}

bool g242WriteOverlapsDirtyDepth(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g242GpuDepthOn())
        return false;
    for (const auto &kv : g_g242DepthStates)
    {
        const G242DepthState &s = kv.second;
        // A CPU write anywhere in this logical Z surface makes the full-surface guest generation
        // diverge.  Even when it misses the dirty row interval, publish that interval first so the
        // subsequent clean mismatch can be handled by a normal full upload (never an invariant
        // failure or a lossy merge).
        if (!s.dirty)
            continue;
        // G471: build this state's Z-surface page set as a 512-BIT set (64 bytes, one cache line)
        // and test the incoming write rect against it, returning at the first hit. Same pages, same
        // +1 slack, same verdict as the old pair of zeroed 512-BYTE arrays plus a full 512-entry
        // scan -- see g471MarkRectBits / g471RectHitsBits.
        G471PageBits bits;
        g471ClearPageBits(bits);
        g471MarkRectBits(bits, s.zbpPage << 5u, s.fbw, GS_PSM_Z24,
                         0u, 0u, static_cast<uint32_t>(s.fbW), 512u);
        if (g471RectHitsBits(bits, bpBlocks, bw64, psm, x, y, w, h))
            return true;
    }
    return false;
}
} // namespace

void g242PrepareVramReadAll(uint8_t *vram)
{
    if (!g242SyncAllDepth(vram))
        std::fprintf(stderr, "[G242:barrier] FAIL all-depth materialization\n");
}

bool g242GuestDepthEnabled()
{
    return g242GpuDepthOn();
}

void g242PrepareVramReadRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g242SyncOverlappingDepth(vram, bpBlocks, bw64, psm, x, y, w, h))
        std::fprintf(stderr, "[G242:barrier] FAIL read bp=0x%x psm=0x%x\n", bpBlocks, psm);
}

// Extern hook called immediately before upload/local-transfer writes and immediately after CPU
// raster writes.  Pre-write callers materialize aliased GPU-owned depth; every caller bumps the
// page generations so a clean persistent depth texture is re-uploaded on its next use.
void g178NoteVramWriteRectTagged(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64,
                                 uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                 uint32_t h, uint32_t g570Source)
{
    if (!g178GpuOn())
        return;
    if (g242WriteOverlapsDirtyDepth(bpBlocks, bw64, psm, x, y, w, h))
        g242PrepareVramReadRect(vram, bpBlocks, bw64, psm, x, y, w, h);
    g178BumpRectImpl(bpBlocks, bw64, psm, x, y, w, h, g570Source);
}

void g178NoteVramWriteRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    g178NoteVramWriteRectTagged(vram, bpBlocks, bw64, psm, x, y, w, h, 0u);
}

// ===== G261 GPU-RESIDENT RTT WAVES (DEFAULT-ON since G266; see g261WaveOn above) =====
// Residency state for the five G248 RTT targets. `dirty` = the target's FBO holds rows guest
// VRAM does not (a wave rendered with readback skipped). Ownership contract: every CPU
// read/write of those pages must call a materialize hook FIRST; the hooks below cover the four
// real consumer edges (CPU band replay, inline primitive, VRAM transfers, local→host reads).
// All state is touched only on the threads that already own the G144 flush machinery (GS worker
// under MTGS / EE thread at the frame barrier — never concurrently), same as g_g178FbSnap.
struct G261Res
{
    bool dirty = false;
    int dirtyLo = 512, dirtyHi = -1; // GS rows the FBO has and VRAM lacks
    // G662: the COLUMN union of the scissors of every wave that dirtied this target, i.e. the
    // horizontal analogue of `dirtyLo/dirtyHi`. The publication readback is
    // `g178_backend_read_color(k, r.fbW, ...)` — always the FULL target width — and P6 measured
    // that readback at 80.05% of the whole `kG261MatLocal` cost, so this is where Rule 11
    // (producer-bounded transfers) has an unexamined axis. Maintained and read only under
    // PS2X_G662_DIAG for now: `[G662:colspan]` PRICES the ceiling before anything narrows.
    int dirtyColLo = 1 << 24, dirtyColHi = -1;
    int covLo = 512, covHi = -1;     // GS rows DEFINED in the FBO (upload/readback/render union)
    uint32_t fbw = 0;
    int fbW = 0;
    uint64_t genAtSkip = 0;          // gen sum over the ROW WINDOW when readback was last skipped
    // Row-window-scoped residency footprint. A whole-target (512-row) range is refuted: at RTT
    // block density (targets 3 pages apart, texture pages ~0x34e0 close behind 0x2720) it
    // swallows the neighbor targets and the per-frame texture uploads, materializing every wave
    // within milliseconds (G261 bring-up measured mat.up=409/window from exactly this).
    int winLo = 512, winHi = -1;     // rows the residency claims (cov ∪ dirty)
    G144BlockRange range{};          // block range of the claimed row window (overlap tests)
    // G264: rows the guest re-uploaded into VRAM while this target was resident, deferred for
    // an address-exact VRAM→FBO patch at the next consumer/flush edge instead of a materialize
    // round-trip. Per-pixel bits are required because DC2 intentionally aliases the same pages
    // through different DBP/DBW and FRAME/FBW layouts; a source rect is not generally a target
    // rect, and coalescing disjoint uploads into a row bounding box can clobber dirty FBO pixels.
    bool mirPending = false;
    int mirLo = 512, mirHi = -1;
    std::array<std::array<uint64_t, 8>, 512> mirBits{}; // max target width = 512 pixels
    // G291: cumulative per-pixel "upload re-covered since the last wave write to this row" map
    // (same tx/ty geometry as mirBits, but NOT cleared by the mirror flush). A 64x32 CT32 page
    // tile whose every pixel bit is set has been fully re-written in guest VRAM by noted
    // uploads AFTER the last GPU render of those rows, so VRAM >= FBO for that page and a
    // texture/CLUT read overlapping only such pages needs no read-triggered materialization
    // (the materialize round-trip — mirror VRAM->FBO then readback FBO->VRAM — is the identity
    // on VRAM for a fully re-covered page). Bits are cleared on every wave render of the rows,
    // on any residency reset/width flip, and are meaningful only while g291CovFbw == fbw.
    std::array<std::array<uint64_t, 8>, 512> g291Cov{};
    uint32_t g291CovFbw = 0;
    // G536: PER-PAGE write generation at the moment this residency last claimed its window
    // (the same instant `genAtSkip` is anchored). `genAtSkip` is a SUM over the whole window and
    // is only ever compared where a NEW wave decides `uploadFb` — it is never re-tested at
    // g261Materialize, i.e. where the FBO is written back OVER guest VRAM. A page whose gen has
    // moved since the anchor holds a guest write that was never reconciled into the FBO, so for
    // THAT page guest VRAM is the newer copy and publishing the FBO over it destroys real bytes.
    // Index: p = pageRow * fbw + pageCol, matching g291PagesCurrent's page walk.
    std::array<uint64_t, 16 * 8> g536PageGen{};
    uint32_t g536PageGenFbw = 0;
};
static constexpr uint32_t kG261Fbp[kG248TargetCount] = {
    // 0..4 legacy five (G248) | 5 G553 | 6 G556 | 7 G557
    0x139u, 0x13cu, 0x143u, 0x146u, 0x155u, 0x159u, 0x181u, 0x13au,
    // 8..13 G569 route3 targets, ordered by CPU-replayed ENTRY weight (the ranking that matters —
    // see g568_rank_fallback_by_entries_not_events), so DC2_G569_RTTWIDE_MASK bisects usefully.
    0x13du, 0x157u, 0x14bu, 0x151u, 0x13fu, 0x141u
};
static G261Res g_g261Res[kG248TargetCount];
static uint32_t g_g261VramSize = 0; // set beside the wave flush (GS::m_vramSize is private)
enum G261MatCause : int
{
    kG261MatCpuReplay = 0,
    kG261MatInline = 1,
    kG261MatUpload = 2,
    kG261MatLocal = 3,
    kG261MatL2H = 4,
    kG261MatTexAlias = 5,
    kG261MatResync = 6,
    kG261MatFail = 7,
};
static std::atomic<uint64_t> g_g261Waves{0}, g_g261SkippedRb{0}, g_g261FboBinds{0},
    g_g261FboRejects{0}, g_g261Invariants{0}, g_g261MatUnknownRange{0};
// G285: page-exact destructive local-copy consumption. The copy replaces every dirty page after
// one partially covered page-row, so only that prefix row needs publication.
static std::atomic<uint64_t> g_g285L2lConsumes{0}, g_g285L2lRowsPublished{0},
    g_g285L2lRowsElided{0};
static std::atomic<uint64_t> g_g263FboBindBi{0}; // G263: bilinear-admitted FBO-direct binds
// G267: display-TRISTRIP bilinear tap/page census. The behavior prototype was neutral after its
// required alias/lifecycle repairs and was removed; these counters remain diagnostics-only.
static std::atomic<uint64_t> g_g267TriCandidates{0}, g_g267TriEligible{0},
    g_g267TriFootReject{0}, g_g267TriAliasReject{0};
// G264 upload-into-FBO mirror telemetry: uploads deferred to a mirror, mirrors executed,
// mirror failures (residency dropped loudly).
static std::atomic<uint64_t> g_g264MirNoted{0}, g_g264MirFlushed{0}, g_g264MirFail{0};
