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
static const char *const kG266FrName[] = {
    "none", "off", "cpuonlyrtt", "fbw", "mixedtgt", "classify", "mixedz", "rttdepth",
    "colorzalias", "depthsetup", "texaliasz", "submit", "zreadback"};
static std::map<uint64_t, uint64_t> g_g266CpuShapes;
static std::map<uint64_t, uint64_t> g_g266L2lShapes;
static std::map<uint64_t, uint64_t> g_g266TexShapes;
static std::atomic<uint64_t> g_g266CpuN{0}, g_g266L2lN{0}, g_g266TexN{0};

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
                 g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0)
            st.blend = 3; // (Cs-0)*(FIX=128)>>7 + Cd == Cs + Cd (G262 census shape, RTT waves only)
        else if (a == 2u && b == 0u && c == 2u && d == 1u && ((al >> 32) & 0xFFull) == 128ull &&
                 g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0)
            st.blend = 4; // (0-Cs)*(FIX=128)>>7 + Cd == Cd - Cs (G262 census shape, RTT waves only)
        else if (a == 2u && b == 1u && c == 0u && d == 1u &&
                 g265DisplayZWaveOn() && g265DisplayTarget(ctx.frame.fbp))
            st.blend = 5; // G265 census: (0-Cd)*As>>7 + Cd == Cd*(1-As)
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
        const bool g255RttEntry =
            (g255GpuRttOn() && g248TargetIndex(ctx.frame.fbp) >= 0) || g286ExactEntry;
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
        const bool exactSampler = (g256ExactRttOn() && g255RttEntry) || g286ExactEntry;
        st.wrapU = exactSampler ? wu : (st.bilinear ? wu : 1u);
        st.wrapV = exactSampler ? wv : (st.bilinear ? wv : 1u);
    }

    // G270 line entries deliberately retain drawLine's semantics: no texture and no depth
    // read/write. Alpha test, blend, scissor, and framebuffer validation above still apply. A
    // defensive gate keeps an accidentally captured textured/off-target line on CPU fallback.
    if (g270LineType(static_cast<uint32_t>(e.prim.type)))
    {
        if (!g270LineWaveOn() || e.prim.tme ||
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
            // G262: widened RTT-family entries take the same guestDepth derivation as G242, but
            // the flush executes it as a NON-PERSISTENT per-wave Z round-trip (no ownership).
            const bool g262Rtt = g262WideOn() && g248TargetIndex(ctx.frame.fbp) >= 0;
            const bool g265Display = g265DisplayZWaveOn() && g265DisplayTarget(ctx.frame.fbp);
            if (g242GpuDepthOn() || g262Rtt || g265Display)
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
                     static_cast<uint32_t>(rows));
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
    uint32_t pw = 0u, ph = 0u;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t basePage = bpBlocks >> 5u;
    const uint32_t px0 = x / pw, px1 = (x + w - 1u) / pw;
    const uint32_t py0 = y / ph, py1 = (y + h - 1u) / ph;
    for (uint32_t py = py0; py <= py1 && py - py0 < kG178PageCount; ++py)
        for (uint32_t px = px0; px <= px1 && px - px0 < kG178PageCount; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            pages[pg & (kG178PageCount - 1u)] = 1u;
            pages[(pg + 1u) & (kG178PageCount - 1u)] = 1u;
        }
}

bool g242RectsOverlap(uint32_t aBp, uint32_t aBw, uint32_t aPsm,
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
        if (s.dirty && g242RectsOverlap(s.zbpPage << 5u, s.fbw, GS_PSM_Z24,
                                        0u, 0u, static_cast<uint32_t>(s.fbW), 512u,
                                        bpBlocks, bw64, psm, x, y, w, h))
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
void g178NoteVramWriteRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g178GpuOn())
        return;
    if (g242WriteOverlapsDirtyDepth(bpBlocks, bw64, psm, x, y, w, h))
        g242PrepareVramReadRect(vram, bpBlocks, bw64, psm, x, y, w, h);
    g178BumpRectImpl(bpBlocks, bw64, psm, x, y, w, h);
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
};
static constexpr uint32_t kG261Fbp[kG248TargetCount] = {0x139u, 0x13cu, 0x143u, 0x146u, 0x155u};
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