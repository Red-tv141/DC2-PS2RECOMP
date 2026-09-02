// ===== G623: PRODUCER-SURFACE DIRECT CONSUMPTION ================================================
//
// THE OBJECT. `[G620:texdec]` on the shipped G622 binary, `dungeon1`:
//
//   #00 key=d5bcd3..  512x512 psm= 0 (CT32) tbp=0x02760 tbw=8 | dec=15075 distinct=15075
//                     pxPerFlush=20582 of decPxPerFlush=25342  ==>  81.2% of ALL decoded texels
//   #02 key=ba04eb..  512x512 psm= 1 (CT24) tbp=0x02760 tbw=8 | dec=  557 pxPerFlush=760 (3.0%)
//
// `tbp=0x02760` is `framePageBaseToBlock(0x13b)` — G286's TRANSIENT post-process target. Its life
// today is: rendered on the GPU -> read back through a fence -> unpacked into guest VRAM -> hashed
// -> decoded on the CPU -> re-uploaded to the GPU -> sampled by a full-screen display sprite
// (`[G600:big] dst=0x0 tbp=0x2760 psm=0 tbw=8 prim=6 fst=1 tex=512x512 | src=0x0 view=0`).
// `distinct == dec` on every one of those decodes: the content genuinely changes every frame, so no
// content cache can ever answer it. The only way to delete the work is to stop MATERIALIZING the
// surface for a consumer that is itself on the GPU.
//
// WHY IT IS NOT ALREADY BOUND. `G178Draw::srcFbp` exists and the backend honours it for any fbp in
// `m_fbos`, but the front-end only ever sets it for a RESIDENT target (`kG261Fbp`), whose residency
// record supplies fbW/covLo/covHi. 0x13b is deliberately NOT resident (G621 refused it: it renders
// inside 0x139's VRAM footprint and an ownership claim there is G400's alias family).
//
// THE CLAIM THIS UNIT MAKES IS STRICTLY WEAKER THAN RESIDENCY, AND THAT IS THE POINT.
// It never claims ownership, never defers a readback and never leaves guest VRAM stale. The
// transient contract is untouched: the producer batch still publishes every row it rendered into
// guest VRAM in its own flush. All this unit adds is a RECORD that, at the instant of that
// publication, `FBO(fbp) == VRAM(pages of fbp)` — and a test, taken at the consumer, that nothing
// has written those pages since. If nothing has, a VRAM decode of those pages IS the FBO's texels,
// so sampling the FBO is the identity and the decode + upload are pure waste.
//
// THE AUTHORITY CONDITIONS, all of which must hold (`[G623:pc]` ranks the ones that fail):
//   version  every 8 KiB page the read touches carries the same `g_g178PageGen` value it carried
//            when this surface was last published. That array is the SAME one the shipped texture
//            registry's gen tier already bets a stale-texture defect on, at the same granularity,
//            for this same texture key — this unit introduces no new invariant.
//   alias    no OTHER resident target is dirty over the pages the read touches. A dirty target has
//            pixels in ITS FBO that VRAM lacks and that no gen bump records, so the gen test alone
//            cannot see them. The legacy path materializes such a target before decoding; this unit
//            refuses instead (fail closed).
//   format   PSMCT32 read at the producer's own base and BW — the identity interpretation of the
//            producer's RGBA8 pixels. CT24/T8/T4 views of a producer are a different function of
//            the same bytes and are refused here (G338/G281 are the shapes that answer those).
//   window   the sampled texel extent lies inside the rows the FBO is DEFINED over
//            (`covLo..covHi`, the union of published bands that are still gen-clean) and inside
//            `fbW`. NEAREST uses the integer extent; BILINEAR uses the exact GL tap footprint
//            (G263's rule, re-derived here against the surface record instead of a G261Res).
//   liveness the surface's geometry still matches the batch that produced it, and the consumer is
//            not the producer (no feedback loop).
//
// ORACLE. `DC2_G623_VERIFY=1` records a would-be bind but DOES NOT TAKE IT: the decode runs anyway
// and the producer FBO's covered window is read back and compared against the decoded texels. A
// verify run therefore always ships the pre-G623 bytes — it can report a defect, never cause one —
// and it is strictly slower than either arm, so it is never valid in a timing run.
//
// Rollback DC2_G623_NO_PRODSRC=1 · census DC2_G623_PC=1 · oracle DC2_G623_VERIFY=1
// · paired arm DC2_G419_AB=prodsrc.
// ==============================================================================================

// A produced surface can be at most 512x512 CT32 = 1 MiB = 128 pages of 8 KiB.
static constexpr uint32_t kG623Pages = 128u;

struct G623Surface
{
    uint32_t fbp = 0u;
    uint32_t fbBlock = 0u; // framePageBaseToBlock(fbp)
    uint32_t basePage = 0u;
    uint32_t fbw = 0u;
    int fbW = 0, fbH = 0;
    int covLo = 512, covHi = -1; // GS rows over which FBO == VRAM was PROVEN at `gen`
    uint64_t gen[kG623Pages] = {};
    uint32_t pages = 0u;
    uint64_t seq = 0u;
    bool valid = false;
};

// Flush-owner (GS worker) thread only — the same contract as `g_g178FbSnap` and `g_g178TexReg`.
static std::unordered_map<uint32_t, G623Surface> g_g623Surf;
static uint64_t g_g623Seq = 0u;

// ⭐⭐⭐ DEFAULT-ON, PROMOTED BY G624. Rollback `DC2_G623_NO_PRODSRC=1`.
//
// G623 built this, proved it exact in TEXELS (14,336 oracle checks over 3.05 Gpx, `bad=0`) and
// priced it at −0.850 / −0.883 ms/f paired on `dungeon1` — and then its 4-arm pixel gate FAILED at
// 5.2% byte-identical against a 76.8/77.9% same-binary control band. The cause was named there: the
// shipped G406/G407 GS 12.4 SPRITE UV ROUNDING (`uvOriginY`/`uvRoundV`, shader `mode` bits 16384
// and 4096) is a statement about the GS V axis, and the `tq = 1 - tq` flip into the producer FBO's
// bottom-first space inverts both its tie-break and both its nudge directions.
//
// G624 repaired that IN THE FRAGMENT PROGRAM (`uUvFlipV`): undo the flip, evaluate the identical
// rounding function on the GS-space value, re-flip the result. The same 4-arm gate then read
// **76.6% / 75.3% identical against a 74.3–77.8% control band** — inside it, with the lever pairs'
// median |d| (4.79 / 4.44) BELOW both controls' (8.58 / 12.04). Full evidence: phase-G624 fix log.
static bool g623ProdSrcOn()
{
    // G624: the G560 multidraw build packs its per-primitive state into `vG560State` and has no
    // slot left for the flip flag, so its copy of the fragment program keeps the pre-G624
    // (unflipped) G406 rounding. That is wrong for a producer-FBO bind by construction, so refuse
    // to arm rather than ship the defect G623's pixel gate caught. G560 multidraw is opt-in.
    static const bool on = !envFlagEnabled("DC2_G623_NO_PRODSRC") &&
                           !(envFlagEnabled("DC2_G560_MULTI_DRAW") &&
                             !envFlagEnabled("DC2_G560_NO_MULTI_DRAW"));
    return on;
}

#if defined(PS2X_G684_HOT_DIAG)
static bool g623CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G623_PC");
    return on;
}

static bool g623VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G623_VERIFY");
    return on;
}

static bool g623Paired()
{
    static const bool paired = []() {
        const char *v = std::getenv("DC2_G419_AB");
        return v != nullptr && std::strcmp(v, "prodsrc") == 0;
    }();
    return paired;
}
#else
static constexpr bool g623CensusOn() { return false; }
static constexpr bool g623VerifyOn() { return false; }
static constexpr bool g623Paired() { return false; }
#endif

// ENV-LEVEL selection, arm-INDEPENDENT. The registry STAMP and the whole census run under this, so
// both paired buckets maintain identical state and only the BIND differs (G520 §7b).
static bool g623Tracked()
{
    static const bool on = g623CensusOn() || g623VerifyOn() || g623Paired() || g623ProdSrcOn();
    return on;
}

// Per-call: honours the paired arm, which flips every A/B tick and must never be cached.
// Under the oracle the bind is never TAKEN (the decode must run so there is something to compare).
static bool g623BindActive()
{
    if (!g623Tracked() || g623VerifyOn())
        return false;
    if (g623Paired())
        return g419AbArmFor(static_cast<int>(kG623LeverProdSrc), g623ProdSrcOn());
    return g623ProdSrcOn();
}

// ---- the registry ------------------------------------------------------------------------------

// TRUE iff every page slot the surface covers still carries the generation it was stamped with.
static bool g623PagesClean(const G623Surface &s, uint32_t slotLo, uint32_t slotHi)
{
    if (slotHi >= s.pages)
        slotHi = s.pages ? s.pages - 1u : 0u;
    for (uint32_t p = slotLo; p <= slotHi; ++p)
        if (g_g178PageGen[(s.basePage + p) & (kG178PageCount - 1u)].load(
                std::memory_order_relaxed) != s.gen[p])
            return false;
    return true;
}

// Slot span of a GS row band, in the surface's own page geometry. Carries g178BumpRectImpl's
// conservative +1 slack slot exactly as g591ColSlotHi does.
static inline uint32_t g623SlotLo(int rowLo, uint32_t fbw)
{
    return (static_cast<uint32_t>(rowLo < 0 ? 0 : rowLo) / 32u) * fbw;
}
static inline uint32_t g623SlotHi(int rowHi, uint32_t fbw)
{
    return (static_cast<uint32_t>(rowHi > 511 ? 511 : rowHi) / 32u) * fbw + fbw;
}

// Called at the ONE instant the colour readback has just written rows [rowLo,rowHi] of this fbp's
// FBO back into guest VRAM, i.e. the only instant at which FBO == VRAM is KNOWN for those rows.
// The previous coverage survives the union iff its own pages are still clean (g591PrivZStamp's
// rule); otherwise coverage resets to this flush's band.
static void g623NotePublished(uint32_t fbp, uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                              int rowLo, int rowHi, bool published)
{
    if (!g623Tracked() || !published || rowHi < rowLo || fbw == 0u || fbw > 8u || fbW <= 0 ||
        fbH <= 0)
        return;
    // Resident targets keep G261's residency path; the display buffers have their own (0x68 is a
    // reserved srcFbp token in the draw pass). What is left is exactly the TRANSIENT population.
    if (g248TargetIndex(fbp) >= 0 || fbp == 0u || fbp == 0x68u)
        return;
    G623Surface &s = g_g623Surf[fbp];
    const uint32_t basePage = fbBlock >> 5;
    const uint32_t pages = std::min(kG623Pages, 16u * fbw);
    int lo = rowLo, hi = rowHi;
    if (s.valid && s.basePage == basePage && s.fbw == fbw && s.fbW == fbW && s.fbH == fbH &&
        s.covHi >= s.covLo &&
        g623PagesClean(s, g623SlotLo(s.covLo, fbw), g623SlotHi(s.covHi, fbw)))
    {
        lo = std::min(lo, s.covLo);
        hi = std::max(hi, s.covHi);
    }
    s.fbp = fbp;
    s.fbBlock = fbBlock;
    s.basePage = basePage;
    s.fbw = fbw;
    s.fbW = fbW;
    s.fbH = fbH;
    s.covLo = lo;
    s.covHi = hi;
    s.pages = pages;
    s.seq = ++g_g623Seq;
    s.valid = true;
    for (uint32_t p = 0; p < kG623Pages; ++p)
        s.gen[p] = (p < pages) ? g_g178PageGen[(basePage + p) & (kG178PageCount - 1u)].load(
                                     std::memory_order_relaxed)
                               : 0u;
}

static inline bool g623HaveSurfaces()
{
    return !g_g623Surf.empty();
}

// The consumer's base -> surface lookup. Keyed on the texture BASE BLOCK, not on the fbp, because
// that is what the consumer names.
static const G623Surface *g623SurfaceForBase(uint32_t tbp0, uint32_t tbw)
{
    for (const auto &kv : g_g623Surf)
    {
        const G623Surface &s = kv.second;
        if (s.valid && s.fbBlock == tbp0 && s.fbw == tbw)
            return &s;
    }
    return nullptr;
}

// Consumed by the DRAW pass, which must push UVs against the PRODUCER's fbW x 512 space.
static int g623SurfaceFbW(uint32_t fbp)
{
    const auto it = g_g623Surf.find(fbp);
    return (it != g_g623Surf.end() && it->second.valid) ? it->second.fbW : 0;
}

// ===== G624: the BLAST RADIUS of the flip-aware G406/G407 V rounding ============================
//
// The repair lives in the fragment program (`uUvFlipV`), so it applies to EVERY `srcFbp`-bound
// draw that carries rounding flags — not only to G623's producer consumer. G623's fix log asserted
// from source reading that no OTHER `srcFbp` consumer is a G406-rounded sprite ("a resident-target
// read taken by a TRIANGLE/TRISTRIP path or a non-G406-rounded sprite"). That assertion decides
// whether this change is a repair with a 1-draw footprint or a pixel change on every resident
// target on the route, so it is COUNTED rather than believed.
//
//   bound   `srcFbp`-bound draws pushed this run
//   round   ... of which carry (flagsU|flagsV) != 0, i.e. reach `mode & 16384`
//   prod    ... of those, whose source is a G623 PRODUCER SURFACE (the population G623 failed on)
//   res     ... of those, whose source is an ordinary RESIDENT target or token — the regression
//               surface. `res=0` is the fix log's claim.
//
// `DC2_G624_UVFLIP_STAT=1`.
static std::atomic<uint64_t> g_g624Bound{0}, g_g624Round{0}, g_g624RoundProd{0}, g_g624RoundRes{0};
static std::atomic<uint64_t> g_g624FlagV{0}, g_g624FlagU{0};

static bool g624StatOn()
{
    static const bool on = envFlagEnabled("DC2_G624_UVFLIP_STAT");
    return on;
}

static void g624Print()
{
    const uint64_t bound = g_g624Bound.load(std::memory_order_relaxed);
    const uint64_t round = g_g624Round.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G624:uvflip] bound=%llu round=%llu (%.2f%%) prod=%llu res=%llu | "
                 "flagsU!=0=%llu flagsV!=0=%llu\n",
                 static_cast<unsigned long long>(bound),
                 static_cast<unsigned long long>(round),
                 bound != 0u ? 100.0 * static_cast<double>(round) / static_cast<double>(bound)
                             : 0.0,
                 static_cast<unsigned long long>(g_g624RoundProd.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g624RoundRes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g624FlagU.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g624FlagV.load(std::memory_order_relaxed)));
}

// `producer` is decided by the caller, which already knows whether `g248TargetIndex` resolved.
static void g624NoteBound(uint32_t flagsU, uint32_t flagsV, bool producer)
{
    if (!g624StatOn())
        return;
    const uint64_t n = g_g624Bound.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((flagsU | flagsV) != 0u)
    {
        g_g624Round.fetch_add(1u, std::memory_order_relaxed);
        (producer ? g_g624RoundProd : g_g624RoundRes).fetch_add(1u, std::memory_order_relaxed);
        if (flagsU != 0u)
            g_g624FlagU.fetch_add(1u, std::memory_order_relaxed);
        if (flagsV != 0u)
            g_g624FlagV.fetch_add(1u, std::memory_order_relaxed);
    }
    if ((n % 20000u) == 0u)
        g624Print();
}

// ---- census ------------------------------------------------------------------------------------
// Blocked-conjunct ranking, taken BEFORE the gate (g513), so "the lever never fired" stays separable
// from "the lever fired and found nothing".
enum G623Blk : int
{
    kG623BlkAlreadyBound = 0, // an earlier bind path already claimed this entry
    kG623BlkFormat,        // psm is not PSMCT32 (CT24 / T8 / T4 views are a different function)
    kG623BlkFeedback,      // the consumer IS the producer
    kG623BlkPrim,          // non-FST, or a primitive class the UV remap does not cover
    kG623BlkTexDims,       // declared texture dims exceed the producer surface
    kG623BlkWindow,        // the sampled extent leaves the FBO-defined rows/columns
    kG623BlkBilinTri,      // bilinear TRIANGLE — no exact tap proof built yet
    kG623BlkPageMoved,     // a page the read touches has been written since publication
    kG623BlkAliasDirty,    // a dirty resident target overlaps the sampled pages
    kG623BlkN,
};
static const char *const kG623BlkName[kG623BlkN] = {
    "alreadyBound", "format",   "feedback",  "prim",
    "texDims",      "window",   "bilinTri",  "pageMoved", "aliasDirty"};

static std::atomic<uint64_t> g_g623N{0}, g_g623Clean{0}, g_g623Binds{0}, g_g623BindPx{0};
static std::atomic<uint64_t> g_g623Blk[kG623BlkN]{};
static std::atomic<uint64_t> g_g623Unconsidered{0}, g_g623UnconsideredPx{0};
static std::atomic<uint64_t> g_g623VfyChecks{0}, g_g623VfyPixels{0}, g_g623VfyBad{0},
    g_g623VfyRowBad{0}, g_g623VfyFail{0};

// Per-FLUSH desired bind, by entry index. ORACLE ONLY: populated only under DC2_G623_VERIFY, so the
// shipped path never touches it.
static std::vector<uint32_t> g_g623Want;

static inline void g623ResetFlush(size_t n)
{
    if (!g623VerifyOn())
        return;
    g_g623Want.assign(n, 0u);
}

static void g623Print()
{
    char blk[320];
    int bo = 0;
    blk[0] = '\0';
    for (int i = 0; i < kG623BlkN; ++i)
    {
        const uint64_t v = g_g623Blk[i].load(std::memory_order_relaxed);
        if (v != 0u && bo < 280)
            bo += std::snprintf(blk + bo, sizeof(blk) - bo, " %s=%llu", kG623BlkName[i],
                                static_cast<unsigned long long>(v));
    }
    const uint64_t n = g_g623N.load(std::memory_order_relaxed);
    const uint64_t clean = g_g623Clean.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G623:pc] n=%llu CLEAN=%llu (%.2f%%) binds=%llu bindMpx=%.2f surfaces=%zu | "
                 "unconsidered=%llu (%.2f Mpx) | blocked:%s | verify(checks=%llu px=%llu bad=%llu "
                 "badRows=%llu fail=%llu)\n",
                 static_cast<unsigned long long>(n), static_cast<unsigned long long>(clean),
                 n != 0u ? 100.0 * static_cast<double>(clean) / static_cast<double>(n) : 0.0,
                 static_cast<unsigned long long>(g_g623Binds.load(std::memory_order_relaxed)),
                 static_cast<double>(g_g623BindPx.load(std::memory_order_relaxed)) / 1.0e6,
                 g_g623Surf.size(),
                 static_cast<unsigned long long>(
                     g_g623Unconsidered.load(std::memory_order_relaxed)),
                 static_cast<double>(
                     g_g623UnconsideredPx.load(std::memory_order_relaxed)) / 1.0e6,
                 bo ? blk : " none",
                 static_cast<unsigned long long>(g_g623VfyChecks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g623VfyPixels.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g623VfyBad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g623VfyRowBad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g623VfyFail.load(std::memory_order_relaxed)));
}

static void g623Report()
{
    if (!g623CensusOn())
        return;
    static uint64_t s_next = 0u;
    const uint64_t n = g_g623N.load(std::memory_order_relaxed);
    if (n < s_next)
        return;
    s_next = n + 4000u;
    g623Print();
}

// The population the pre-pass NEVER SAW. `g598_t8why`'s lesson one level up: "rejected on shape" and
// "the decision point never ran for this flush" must not share an undifferentiated bucket. Called
// from the texture pass, which is the only per-entry walk outside the pre-pass guard.
static void g623NoteUnconsidered(const G144Entry &e, const G178EntryState &st, bool considered)
{
    if (!g623Tracked() || considered || !e.prim.tme || st.skip || st.texKey == 0u)
        return;
    if (g623SurfaceForBase(e.ctx.tex0.tbp0, e.ctx.tex0.tbw) == nullptr)
        return;
    g_g623Unconsidered.fetch_add(1u, std::memory_order_relaxed);
    g_g623UnconsideredPx.fetch_add(
        static_cast<uint64_t>(std::max(0, st.texW)) * static_cast<uint64_t>(std::max(0, st.texH)),
        std::memory_order_relaxed);
}

// ---- the exact tap window ----------------------------------------------------------------------
// G263's bilinear sprite rule, re-derived against a producer-surface record instead of a G261Res:
// the first and last GL pixel CENTRES the sprite covers, mapped through the affine UV gradient with
// drawSprite's left-edge (0.0) bias, expanded to the two GL_LINEAR taps. The epsilon biases toward
// INCLUDING a tap, i.e. toward rejection, never toward admission.
static bool g623BilinearSpriteWindow(const G144Entry &e, const G178EntryState &st,
                                     const G623Surface &s, int *tap4)
{
    const GSVertex &q0 = e.v[0];
    const GSVertex &q1 = e.v[1];
    int tapLo[2] = {0, 0}, tapHi[2] = {0, 0};
    for (int a = 0; a < 2; ++a)
    {
        const float p0 = a ? q0.y : q0.x, p1 = a ? q1.y : q1.x;
        const float t0 = static_cast<float>(a ? q0.v : q0.u) / 16.0f;
        const float t1 = static_cast<float>(a ? q1.v : q1.u) / 16.0f;
        const float lo = std::min(p0, p1), hi = std::max(p0, p1);
        const float cF = std::ceil(lo - 0.5f) + 0.5f;
        const float cL = std::ceil(hi - 0.5f) - 0.5f;
        if (!(cL >= cF) || p1 == p0)
            return false; // no tap with nonzero weight, or a degenerate axis — fail closed
        const float g = (t1 - t0) / (p1 - p0);
        const float shift = -0.5f * g;
        const float sA = t0 + (cF - p0) * g + shift;
        const float sB = t0 + (cL - p0) * g + shift;
        const float sLo = std::min(sA, sB), sHi = std::max(sA, sB);
        tapLo[a] = static_cast<int>(std::floor(sLo - 0.5f - 0.001f));
        tapHi[a] = static_cast<int>(std::ceil(sHi - 0.5f + 0.001f));
    }
    const bool clampU = st.wrapU == 1u, clampV = st.wrapV == 1u;
    const int maxCol = std::min(s.fbW, st.texW) - 1;
    const int maxRow = std::min(s.covHi, st.texH - 1);
    const bool uOk = (tapLo[0] >= 0 || (clampU && tapLo[0] == -1)) && tapHi[0] <= maxCol;
    const bool vOk = (tapLo[1] >= s.covLo || (clampV && s.covLo == 0 && tapLo[1] == -1)) &&
                     tapHi[1] <= maxRow;
    if (uOk && vOk)
    {
        tap4[0] = std::max(0, tapLo[0]);
        tap4[1] = tapHi[0];
        tap4[2] = std::max(s.covLo, tapLo[1]);
        tap4[3] = tapHi[1];
    }
    return uOk && vOk;
}

// ---- the decision ------------------------------------------------------------------------------
// ALWAYS COUNTS, so the census is the same call the gate makes. Returns the producer fbp to bind, or
// 0. `alreadyBound` is passed rather than tested here so the caller's earlier bind paths keep their
// own precedence.
static uint32_t g623Decide(uint32_t dstFbp, const G144Entry &e, const G178EntryState &st,
                           bool alreadyBound)
{
    if (!g623Tracked() || !e.prim.tme || st.skip || st.texKey == 0u)
        return 0u;
    const G623Surface *sp = g623SurfaceForBase(e.ctx.tex0.tbp0, e.ctx.tex0.tbw);
    // THE DENOMINATOR IS THE POPULATION, not every entry the pre-pass walks (G618: divide a census
    // by the work VISITED *in the population*). An entry whose base is not a produced surface's
    // base is not a round trip this lever could ever delete, so it is not counted at all — folding
    // it in would bury a 100%-blocked conjunct under millions of irrelevant rows.
    if (sp == nullptr)
        return 0u;
    g_g623N.fetch_add(1u, std::memory_order_relaxed);
    int blocked = -1;
    int vLo = 0, vHi = -1, uLo = 0, uHi = -1;
    // Raw integer UV hull across the primitive's vertices — for the reject SAMPLE only, so a
    // `window` rejection reports the extent that actually left the FBO instead of an empty range.
    int rawU0 = 0, rawU1 = -1, rawV0 = 0, rawV1 = -1;
    {
        const int nv = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
        rawU0 = INT32_MAX; rawU1 = INT32_MIN; rawV0 = INT32_MAX; rawV1 = INT32_MIN;
        for (int k = 0; k < nv; ++k)
        {
            const int uu = static_cast<int>(e.v[k].u >> 4);
            const int vv = static_cast<int>(e.v[k].v >> 4);
            rawU0 = std::min(rawU0, uu); rawU1 = std::max(rawU1, uu);
            rawV0 = std::min(rawV0, vv); rawV1 = std::max(rawV1, vv);
        }
    }
    if (alreadyBound)
        blocked = kG623BlkAlreadyBound;
    else if (e.ctx.tex0.psm != GS_PSM_CT32)
        blocked = kG623BlkFormat;
    else if (sp->fbp == dstFbp)
        blocked = kG623BlkFeedback;
    else if (!e.prim.fst ||
             !(e.prim.type == GS_PRIM_SPRITE || e.prim.type == GS_PRIM_TRIANGLE ||
               e.prim.type == GS_PRIM_TRISTRIP || e.prim.type == GS_PRIM_TRIFAN))
        blocked = kG623BlkPrim;
    else if (st.texW <= 0 || st.texH <= 0 || st.texW > sp->fbW || st.texH > sp->fbH ||
             sp->covHi < sp->covLo)
        blocked = kG623BlkTexDims;
    if (blocked < 0)
    {
        const G623Surface &s = *sp;
        if (st.bilinear)
        {
            if (e.prim.type != GS_PRIM_SPRITE)
                blocked = kG623BlkBilinTri;
            else
            {
                int tap[4] = {0, -1, 0, -1};
                if (!g623BilinearSpriteWindow(e, st, s, tap))
                    blocked = kG623BlkWindow;
                else
                {
                    uLo = tap[0]; uHi = tap[1];
                    vLo = tap[2]; vHi = tap[3];
                }
            }
        }
        else
        {
            // NEAREST: the integer texel extent across the primitive's vertices. Sprite endpoints
            // are exclusive right/bottom (max tap = v1-1); a triangle can reach the vertex texel.
            const int nv = (e.prim.type == GS_PRIM_SPRITE) ? 2 : 3;
            int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
            for (int k = 0; k < nv; ++k)
            {
                const int uu = static_cast<int>(e.v[k].u >> 4);
                const int vv = static_cast<int>(e.v[k].v >> 4);
                u0 = std::min(u0, uu); u1 = std::max(u1, uu);
                v0 = std::min(v0, vv); v1 = std::max(v1, vv);
            }
            const bool sprite = (e.prim.type == GS_PRIM_SPRITE);
            const int hiU = sprite ? u1 - 1 : u1;
            const int hiV = sprite ? v1 - 1 : v1;
            const bool ok = u0 >= 0 && hiU <= std::min(s.fbW, st.texW) - 1 &&
                            v0 >= s.covLo && hiV <= std::min(s.covHi, st.texH - 1) && hiU >= u0 &&
                            hiV >= v0;
            if (!ok)
                blocked = kG623BlkWindow;
            else
            {
                uLo = u0; uHi = hiU;
                vLo = v0; vHi = hiV;
            }
        }
    }
    if (blocked < 0)
    {
        const G623Surface &s = *sp;
        // VERSION. Every page the sampled ROW BAND touches must still carry the generation it was
        // stamped with when this surface was published.
        if (!g623PagesClean(s, g623SlotLo(vLo, s.fbw), g623SlotHi(vHi, s.fbw)))
            blocked = kG623BlkPageMoved;
        else
        {
            // ALIAS. A dirty resident target holds pixels VRAM lacks and no gen bump records them,
            // so the version test cannot see them. Refuse (the legacy path materializes instead).
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
                blocked = kG623BlkAliasDirty;
            else
                for (uint32_t t = 0; t < kG248TargetCount; ++t)
                {
                    const G261Res &r = g_g261Res[t];
                    if (!r.dirty)
                        continue;
                    if (!r.range.valid || g144RangeOverlaps(r.range, tex))
                    {
                        blocked = kG623BlkAliasDirty;
                        break;
                    }
                }
        }
    }
    if (blocked >= 0)
    {
        g_g623Blk[blocked].fetch_add(1u, std::memory_order_relaxed);
        // A ranked count says WHICH conjunct blocks; it cannot say by how much. Sample the shape
        // itself so a `window` rejection can be read as "the sprite samples 32 rows past covHi"
        // rather than guessed at.
        if (g623CensusOn())
        {
            static std::atomic<uint64_t> s_log{0};
            const uint64_t ln = s_log.fetch_add(1u, std::memory_order_relaxed);
            if (ln < 24u || (ln % 20000u) == 0u)
                std::fprintf(stderr,
                             "[G623:rej] n=%llu why=%s sf=%u dst=0x%x tbp=0x%x psm=%u tbw=%u "
                             "prim=%u fst=%u bilin=%u wrap=%u%u tex=%dx%d | src=0x%x fbW=%d "
                             "cov=%d..%d uv=%d..%d,%d..%d\n",
                             static_cast<unsigned long long>(ln), kG623BlkName[blocked],
                             g_dc2ScriptFrame.load(std::memory_order_relaxed), dstFbp,
                             e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.psm),
                             e.ctx.tex0.tbw, static_cast<unsigned>(e.prim.type),
                             e.prim.fst ? 1u : 0u, st.bilinear ? 1u : 0u,
                             static_cast<unsigned>(st.wrapU), static_cast<unsigned>(st.wrapV),
                             st.texW, st.texH, sp->fbp, sp->fbW, sp->covLo, sp->covHi,
                             rawU0, rawU1, rawV0, rawV1);
        }
        return 0u;
    }
    g_g623Clean.fetch_add(1u, std::memory_order_relaxed);
    return sp->fbp;
}

// The gate. Records the desired bind for the oracle and returns the fbp to bind (0 under verify).
static uint32_t g623TryBind(uint32_t dstFbp, size_t entryIndex, const G144Entry &e,
                            const G178EntryState &st, bool alreadyBound)
{
    const uint32_t src = g623Decide(dstFbp, e, st, alreadyBound);
    if (src == 0u)
        return 0u;
    if (g623VerifyOn())
    {
        if (entryIndex < g_g623Want.size())
            g_g623Want[entryIndex] = src;
        return 0u; // record but never take: the decode must run so there is something to compare
    }
    if (!g623BindActive())
        return 0u;
    g_g623Binds.fetch_add(1u, std::memory_order_relaxed);
    g_g623BindPx.fetch_add(static_cast<uint64_t>(st.texW) * static_cast<uint64_t>(st.texH),
                           std::memory_order_relaxed);
    return src;
}

// ---- the oracle --------------------------------------------------------------------------------
// The caller decoded the texture from guest VRAM anyway. Read the producer FBO's covered window back
// and prove it already holds exactly those texels. Row-granular as well as pixel-granular, because
// a bind is row-granular (G590's rowEq law). `checks=` is printed beside `bad=` so a run that
// compared nothing can never look like one that passed (G589).
static void g623VerifyDecode(size_t entryIndex, const G178EntryState &st,
                             const std::vector<uint32_t> &px)
{
    if (!g623VerifyOn() || entryIndex >= g_g623Want.size())
        return;
    const uint32_t fbp = g_g623Want[entryIndex];
    if (fbp == 0u)
        return;
    const auto it = g_g623Surf.find(fbp);
    if (it == g_g623Surf.end() || !it->second.valid)
        return;
    const G623Surface &s = it->second;
    const int loRow = std::max(0, s.covLo);
    const int hiRow = std::min(s.covHi, st.texH - 1);
    const int cols = std::min(s.fbW, st.texW);
    if (hiRow < loRow || cols <= 0)
        return;
    const int rows = hiRow - loRow + 1;
    const int glY = s.fbH - 1 - hiRow;
    static std::vector<uint32_t> s_px; // flush-owner thread only
    if (glY < 0 || !g178_backend_read_color(fbp, s.fbW, s.fbH, glY, rows, s_px) ||
        s_px.size() != static_cast<size_t>(s.fbW) * static_cast<size_t>(rows) ||
        px.size() < static_cast<size_t>(st.texW) * static_cast<size_t>(st.texH))
    {
        g_g623VfyFail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    uint64_t bad = 0, rowBad = 0, tot = 0;
    for (int y = loRow; y <= hiRow; ++y)
    {
        // g178_backend_read_color returns GL (bottom-first) rows starting at glY, so buffer row k
        // is GS row (hiRow - k).
        const size_t fboRow = static_cast<size_t>(hiRow - y) * static_cast<size_t>(s.fbW);
        const size_t texRow = static_cast<size_t>(y) * static_cast<size_t>(st.texW);
        uint64_t rb = 0;
        for (int x = 0; x < cols; ++x)
        {
            ++tot;
            if (s_px[fboRow + static_cast<size_t>(x)] != px[texRow + static_cast<size_t>(x)])
                ++rb;
        }
        bad += rb;
        if (rb != 0u)
            ++rowBad;
    }
    const uint64_t checks = g_g623VfyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
    g_g623VfyPixels.fetch_add(tot, std::memory_order_relaxed);
    const uint64_t totBad = g_g623VfyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    g_g623VfyRowBad.fetch_add(rowBad, std::memory_order_relaxed);
    if (bad != 0u || checks <= 8u || (checks % 256u) == 0u)
        std::fprintf(stderr,
                     "[G623:vfy] checks=%llu bad=%llu badRows=%llu fail=%llu | "
                     "last(fbp=0x%x rows=%d..%d cols=%d tex=%dx%d bad=%llu)\n",
                     static_cast<unsigned long long>(checks),
                     static_cast<unsigned long long>(totBad),
                     static_cast<unsigned long long>(
                         g_g623VfyRowBad.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(
                         g_g623VfyFail.load(std::memory_order_relaxed)),
                     fbp, loRow, hiRow, cols, st.texW, st.texH,
                     static_cast<unsigned long long>(bad));
}
