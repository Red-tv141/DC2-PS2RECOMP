// G401 — Dungeon 6 composite-time surface probe.
//
// G400's draw-entry probe misses command-graph/GPU batches. This hook runs after batch
// preparation and immediately before execution of the first full-screen composite batch:
//   fbp=0x068, SPRITE, TME=1, tbp=0x2720, PSMCT32, MODULATE.
// Pair with DC2_G399_SURFDUMP=1 so guest VRAM is authoritative under native residency.
//
//   DC2_G401_COMPOSITE=1
//   DC2_G401_FROM=<present tick>  (default 0)
//   DC2_G401_TO=<present tick>    (default FROM+8)
//   DC2_G401_PPM=1                dump source/destination PPMs

static bool g401CompositeOn()
{
    static const bool s_on = (std::getenv("DC2_G401_COMPOSITE") != nullptr);
    return s_on;
}

static bool g401IsDefinedTexturePsm(uint8_t psm)
{
    switch (psm & 0x3fu)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_T8:
    case GS_PSM_T4:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

static bool g401SkipReservedTexturePsm(const GSPrimReg &prim, const GSContext &ctx)
{
    static const bool s_skip = (std::getenv("DC2_G401_SKIP_RESERVED_PSM") != nullptr);
    return s_skip && prim.tme && !g401IsDefinedTexturePsm(ctx.tex0.psm);
}

static int g401CompositeKind(const G144Entry &e)
{
    const float dx = std::fabs(e.v[1].x - e.v[0].x);
    const float dy = std::fabs(e.v[1].y - e.v[0].y);
    if ((e.ctx.frame.fbp != 0x000u && e.ctx.frame.fbp != 0x068u) ||
        !e.prim.tme || e.ctx.tex0.tbp0 != 0x2720u)
        return 0;
    if (e.ctx.tex0.psm == 0x00u &&
        e.prim.type == GS_PRIM_TRISTRIP &&
        ((e.ctx.zbuf >> 32u) & 1u) != 0u &&
        (e.ctx.test & 0x7ffffu) == 0x5000au)
        return 1; // PSMCT32 scene copy.
    if (e.ctx.tex0.psm == 0x01u && e.prim.type == GS_PRIM_SPRITE &&
        dx > 511.0f && dx < 513.0f && dy > 415.0f && dy < 417.0f)
        return 2; // PSMCT24 destination fade.
    return 0;
}

static bool g401IsCompositeEntry(const G144Entry &e)
{
    return g401CompositeKind(e) != 0;
}

static void g401NoteFrontDoor(GSRasterizer *self, GS *gs, uint8_t *vram,
                              const GSVertex &v0, const GSPrimReg &prim,
                              const GSContext &ctx)
{
    if (!g401CompositeOn())
        return;

    extern std::atomic<uint64_t> g_dc2PresentTick;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    static const uint64_t s_from = g400EnvU64("DC2_G401_FROM", 0ull);
    static const uint64_t s_to = g400EnvU64("DC2_G401_TO", s_from + 8ull);
    if (tick < s_from || tick > s_to)
        return;

    if (gs && vram && prim.tme &&
        (ctx.frame.fbp == 0x000u || ctx.frame.fbp == 0x068u) &&
        ctx.tex0.psm == GS_PSM_T8 &&
        (ctx.tex0.tbp0 == 0x2720u || ctx.tex0.tbp0 == 0x2820u))
    {
        static std::mutex s_t8Mutex;
        static uint64_t s_t8Tick = ~0ull;
        std::lock_guard<std::mutex> lock(s_t8Mutex);
        if (s_t8Tick != tick)
        {
            s_t8Tick = tick;
            const uint32_t bw = std::max(1u, static_cast<uint32_t>(ctx.tex0.tbw) >> 1u);
            uint64_t sum = 0u;
            uint32_t maxL = 0u;
            for (uint32_t y = 0; y < 256u; y += 4u)
                for (uint32_t x = 0; x < 256u; x += 4u)
                {
                    const uint8_t idx = static_cast<uint8_t>(
                        GSMem::ReadP8(vram, ctx.tex0.tbp0, bw, x, y));
                    const uint32_t c = self->lookupCLUT(
                        gs, idx, ctx.tex0.cbp, ctx.tex0.cpsm,
                        ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                    const uint32_t l = ((c & 0xffu) + ((c >> 8u) & 0xffu) +
                                        ((c >> 16u) & 0xffu)) / 3u;
                    sum += l;
                    maxL = std::max(maxL, l);
                }
            std::fprintf(stderr,
                         "[G401:t8-front] tick=%llu fbp=%03x tbp=%04x tbw=%u cbp=%04x "
                         "meanL=%.2f maxL=%u v0rgba=%u,%u,%u,%u\n",
                         static_cast<unsigned long long>(tick), ctx.frame.fbp,
                         ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), ctx.tex0.cbp,
                         static_cast<double>(sum) / 4096.0, maxL,
                         static_cast<unsigned>(v0.r), static_cast<unsigned>(v0.g),
                         static_cast<unsigned>(v0.b), static_cast<unsigned>(v0.a));
        }
    }

    if (prim.type != GS_PRIM_SPRITE || !prim.tme)
        return;

    static std::atomic<uint32_t> s_count{0};
    const uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed);
    if (n >= 64u)
        return;

    std::fprintf(stderr,
                 "[G401:front] n=%u tick=%llu fbp=%03x fbpsm=%u tbp=%04x tpsm=%u "
                 "tfx=%u tcc=%u alpha=%08x test=%08x\n",
                 n,
                 static_cast<unsigned long long>(tick),
                 ctx.frame.fbp,
                 ctx.frame.psm,
                 ctx.tex0.tbp0,
                 ctx.tex0.psm,
                 ctx.tex0.tfx,
                 ctx.tex0.tcc,
                 static_cast<uint32_t>(ctx.alpha),
                 static_cast<uint32_t>(ctx.test));
}

static void g401WriteSurfacePpm(const uint8_t *vram, uint32_t fbp, uint64_t tick,
                                const char *name)
{
    if (!vram)
        return;
    char path[160];
    std::snprintf(path, sizeof(path), "captures/g401_%s_%06llu.ppm", name,
                  static_cast<unsigned long long>(tick));
    FILE *fp = std::fopen(path, "wb");
    if (!fp)
        return;
    std::fprintf(fp, "P6\n512 416\n255\n");
    const uint32_t base = GSInternal::framePageBaseToBlock(fbp);
    for (uint32_t y = 0; y < 416u; ++y)
    {
        for (uint32_t x = 0; x < 512u; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(base, 8u, x, y);
            const uint8_t rgb[3] = {vram[off], vram[off + 1u], vram[off + 2u]};
            std::fwrite(rgb, 1u, sizeof(rgb), fp);
        }
    }
    std::fclose(fp);
}

static void g401NoteCompositeBatch(const uint8_t *vram, const G144Entry &e, size_t entries,
                                   const char *stage)
{
    if (!g401CompositeOn() || !g401IsCompositeEntry(e))
        return;

    extern std::atomic<uint64_t> g_dc2PresentTick;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    static const uint64_t s_from = g400EnvU64("DC2_G401_FROM", 0ull);
    static const uint64_t s_to = g400EnvU64("DC2_G401_TO", s_from + 8ull);
    if (tick < s_from || tick > s_to)
        return;

    static std::mutex s_mutex;
    static uint64_t s_lastPreTick[3] = {~0ull, ~0ull, ~0ull};
    static uint64_t s_lastPostTick[3] = {~0ull, ~0ull, ~0ull};
    std::lock_guard<std::mutex> lock(s_mutex);
    const int kind = g401CompositeKind(e);
    uint64_t &lastTick =
        (stage[0] == 'p' && stage[1] == 'o') ? s_lastPostTick[kind] : s_lastPreTick[kind];
    if (tick == lastTick)
        return;
    lastTick = tick;

    double srcL = 0.0, srcA = 0.0, dstL = 0.0, dstA = 0.0;
    uint32_t srcMax = 0u, dstMax = 0u;
    g400SampleSurface(vram, 0x139u, srcL, srcA, srcMax);
    g400SampleSurface(vram, 0x068u, dstL, dstA, dstMax);
    const GSVertex &v0 = e.v[0];
    const GSVertex &v1 = e.v[1];
    std::fprintf(stderr,
                 "[G401:composite-%s] kind=%s tick=%llu entries=%zu src139(L=%.2f max=%u A=%.2f) "
                 "dst068(L=%.2f max=%u A=%.2f) psm=0x%x tfx=%u tcc=%u "
                 "abe=%u fst=%u pabe=%u texa=%u/%u/%u alpha=0x%llx test=0x%llx "
                 "v0xy=(%.1f,%.1f) uv=(%u,%u) rgba=%u,%u,%u,%u "
                 "v1xy=(%.1f,%.1f) uv=(%u,%u) rgba=%u,%u,%u,%u\n",
                 stage, kind == 1 ? "ct32-copy" : "ct24-fade",
                 static_cast<unsigned long long>(tick), entries,
                 srcL, srcMax, srcA, dstL, dstMax, dstA,
                 static_cast<unsigned>(e.ctx.tex0.psm),
                 static_cast<unsigned>(e.ctx.tex0.tfx),
                 static_cast<unsigned>(e.ctx.tex0.tcc),
                 e.prim.abe ? 1u : 0u,
                 e.prim.fst ? 1u : 0u,
                 e.pabe ? 1u : 0u,
                 static_cast<unsigned>(e.texa.ta0),
                 e.texa.aem ? 1u : 0u,
                 static_cast<unsigned>(e.texa.ta1),
                 static_cast<unsigned long long>(e.ctx.alpha),
                 static_cast<unsigned long long>(e.ctx.test),
                 v0.x, v0.y, static_cast<unsigned>(v0.u), static_cast<unsigned>(v0.v),
                 static_cast<unsigned>(v0.r), static_cast<unsigned>(v0.g),
                 static_cast<unsigned>(v0.b), static_cast<unsigned>(v0.a),
                 v1.x, v1.y, static_cast<unsigned>(v1.u), static_cast<unsigned>(v1.v),
                 static_cast<unsigned>(v1.r), static_cast<unsigned>(v1.g),
                 static_cast<unsigned>(v1.b), static_cast<unsigned>(v1.a));
    std::fflush(stderr);

    static const bool s_ppm = (std::getenv("DC2_G401_PPM") != nullptr);
    if (s_ppm)
    {
        char srcName[32], dstName[32];
        std::snprintf(srcName, sizeof(srcName), "%s_src139", stage);
        std::snprintf(dstName, sizeof(dstName), "%s_dst068", stage);
        g401WriteSurfacePpm(vram, 0x139u, tick, srcName);
        g401WriteSurfacePpm(vram, 0x068u, tick, dstName);
    }
}

static void g401NoteCompositeDraw(const uint8_t *vram, const GSContext &ctx,
                                  const GSPrimReg &prim, const GSVertex &v0,
                                  const GSVertex &v1)
{
    if (!g401CompositeOn())
        return;
    G144Entry e{};
    e.ctx = ctx;
    e.prim = prim;
    e.v[0] = v0;
    e.v[1] = v1;
    g401NoteCompositeBatch(vram, e, 1u, "draw");
}

static void g401NoteCompositeEntries(const uint8_t *vram,
                                     const std::vector<G144Entry> &entries,
                                     const char *stage = "pre")
{
    if (!g401CompositeOn())
        return;
    bool found[3] = {false, false, false};
    for (const G144Entry &e : entries)
    {
        const int kind = g401CompositeKind(e);
        if (kind != 0 && !found[kind])
        {
            found[kind] = true;
            g401NoteCompositeBatch(vram, e, entries.size(), stage);
        }
    }
}

static void g401NoteDisplayFlush(const uint8_t *vram,
                                 const std::vector<G144Entry> &entries,
                                 const char *stage)
{
    if (!g401CompositeOn() || !vram || entries.empty())
        return;
    const G144Entry &e = entries.front();
    if (e.ctx.frame.fbp != 0x000u && e.ctx.frame.fbp != 0x068u)
        return;

    extern std::atomic<uint64_t> g_dc2PresentTick;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    static const uint64_t s_from = g400EnvU64("DC2_G401_FROM", 0ull);
    static const uint64_t s_to = g400EnvU64("DC2_G401_TO", s_from + 8ull);
    if (tick < s_from || tick > s_to)
        return;

    static std::mutex s_mutex;
    static uint64_t s_seq = 0u;
    static uint64_t s_active = 0u;
    std::lock_guard<std::mutex> lock(s_mutex);
    const bool post = stage[0] == 'p' && stage[1] == 'o';
    if (!post)
        s_active = ++s_seq;

    double l = 0.0, a = 0.0;
    uint32_t mx = 0u;
    g400SampleSurface(vram, e.ctx.frame.fbp, l, a, mx);
    std::fprintf(stderr,
                 "[G401:display-%s] seq=%llu tick=%llu entries=%zu fbp=%03x "
                 "L=%.2f max=%u A=%.2f e0(prim=%u tme=%u abe=%u fst=%u tbp=%04x "
                 "tpsm=%u tfx=%u tcc=%u alpha=%llx test=%llx zbuf=%llx)\n",
                 stage, static_cast<unsigned long long>(s_active),
                 static_cast<unsigned long long>(tick), entries.size(), e.ctx.frame.fbp,
                 l, mx, a, static_cast<unsigned>(e.prim.type), e.prim.tme ? 1u : 0u,
                 e.prim.abe ? 1u : 0u, e.prim.fst ? 1u : 0u, e.ctx.tex0.tbp0,
                 static_cast<unsigned>(e.ctx.tex0.psm),
                 static_cast<unsigned>(e.ctx.tex0.tfx),
                 static_cast<unsigned>(e.ctx.tex0.tcc),
                 static_cast<unsigned long long>(e.ctx.alpha),
                 static_cast<unsigned long long>(e.ctx.test),
                 static_cast<unsigned long long>(e.ctx.zbuf));
}
