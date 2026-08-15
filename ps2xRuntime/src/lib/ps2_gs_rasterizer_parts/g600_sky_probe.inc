// ===== G600: WHAT DOES THE MAP-15 BACKDROP ACTUALLY SAMPLE? (diagnostic, default OFF) =====
//
// G599 proved the defect is NOT the guest-VRAM bytes at 0x2720 (identical page hashes in the 8/28
// and the 0/28 arm) and NOT the publication that writes them. This phase measured two new facts
// that re-frame the question:
//
//   1. The broken band is FROZEN — `mean|A-B|` between the presented frames of scriptFrame 1980 and
//      2002 is EXACTLY 0.00 over rows 0..119 while the rest of the frame moves by 15.5. Either
//      nothing writes those pixels, or something re-writes them with byte-identical content.
//   2. `DC2_NO_ZTEST=1` makes that same content fill the WHOLE screen, and
//      `DC2_NO_ZTEST=1 + DC2_G400_NO_RESIDENT_139=1` makes it vanish entirely. So it is a real
//      draw whose SOURCE differs between the two arms — not stale colour, and not geometry.
//
// A census of decisions cannot answer "which pixels did this texture paint"; only the pixels can.
// Hence two default-OFF instruments, both keyed on the SCRIPT clock so they line up with -SfDump:
//
//   DC2_G600_TEX=1            arm the census. `[G600:big]` — one line per big textured entry at the
//                             point where its texture SOURCE is final, and `[G600:tex]` at the
//                             legacy VRAM decode, which also writes the decoded texels as a PPM.
//   DC2_G600_TEX_EVERY=<n>    script-frame bucket for the PPM/first-line-per-key (default 22, the
//                             gate's own -SfDump cadence).
//   DC2_G600_TEX_MINPX=<n>    only textures with w*h >= n (default 65536 = 256x256).
//   DC2_G600_TEX_TBP=<hex>    restrict to one texture base (0 = all).
//   DC2_G600_SF_LO/_SF_HI     window (falls back to DC2_G599_SF_LO/_HI).
//
//   DC2_G600_TEXFILL=<hex>    ⛔ CEILING PROBE, knowingly incorrect: replace the DECODED texels of
//                             the named texture base with solid magenta. Whatever turns magenta on
//                             screen is exactly the area that texture paints — the localizer a
//                             bounding-box census cannot give, because it answers in pixels.
//
// Everything here is behaviour-pure unless DC2_G600_TEXFILL is set: every statement is inside a
// `g600TexOn()` / `g600TexFillTbp()` test whose accessor is a cached static.
//
// ⚠️ This runs on the GS worker (flush-owner) thread only — the same thread that owns
// `g_g178TexReg` — so the seen-set below needs no synchronisation, exactly as that map does not.

static bool g600TexOn()
{
    static const bool on = envFlagEnabled("DC2_G600_TEX");
    return on;
}

static uint32_t g600TexEvery()
{
    static const uint32_t v = g599EnvU32("DC2_G600_TEX_EVERY", 22u);
    return v;
}

static uint32_t g600TexMinPx()
{
    static const uint32_t v = g599EnvU32("DC2_G600_TEX_MINPX", 65536u);
    return v;
}

static uint32_t g600TexTbp()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G600_TEX_TBP");
        return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    return v;
}

// ⛔ ceiling probe: 0 = off (a texture base of 0 is the CLUT page, never a backdrop source).
static uint32_t g600TexFillTbp()
{
    static const uint32_t v = []() -> uint32_t {
        const char *e = std::getenv("DC2_G600_TEXFILL");
        return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    return v;
}

// ⛔ CEILING PROBE, knowingly incorrect: paint EVERY decoded texture a flat colour derived from its
// base, so one capture names the texture base of every textured pixel on screen instead of one base
// per run. Also forces a decode on every entry (the registry hit would otherwise keep the original
// texels resident), which is why it is a separate flag from DC2_G600_TEXFILL.
static bool g600TexPaintOn()
{
    static const bool on = envFlagEnabled("DC2_G600_TEXPAINT");
    return on;
}

// Stable, high-contrast, and printable: 12 hues indexed by the base's own bits.
static uint32_t g600PaintColour(uint32_t tbp)
{
    static const uint32_t kPal[12] = {
        0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u,
        0xFF0080FFu, 0xFF80FF00u, 0xFFFF8000u, 0xFF8000FFu, 0xFF00FF80u, 0xFFFF0080u,
    };
    return kPal[(tbp ^ (tbp >> 5) ^ (tbp >> 9)) % 12u];
}

static bool g600InWindow()
{
    static const uint32_t lo = g599EnvU32("DC2_G600_SF_LO", g599EnvU32("DC2_G599_SF_LO", 0u));
    static const uint32_t hi = g599EnvU32("DC2_G600_SF_HI", g599EnvU32("DC2_G599_SF_HI", 0u));
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    return sf >= lo && (hi == 0u || sf <= hi);
}

static bool g600BigTex(const G144Entry &e, int texW, int texH)
{
    if (!e.prim.tme || texW <= 0 || texH <= 0)
        return false;
    if (static_cast<uint32_t>(texW) * static_cast<uint32_t>(texH) < g600TexMinPx())
        return false;
    const uint32_t only = g600TexTbp();
    return only == 0u || only == e.ctx.tex0.tbp0;
}

static uint32_t g600Bucket()
{
    const uint32_t every = g600TexEvery();
    return g_dc2ScriptFrame.load(std::memory_order_relaxed) / (every ? every : 1u);
}

// One report per (script bucket, texture key, kind). Bounded: the set is cleared whenever the
// bucket advances, so it can never grow past one bucket's distinct keys.
static bool g600FirstInBucket(uint64_t key, uint32_t kind)
{
    static std::unordered_set<uint64_t> s_seen;
    static uint32_t s_bucket = 0xFFFFFFFFu;
    const uint32_t bucket = g600Bucket();
    if (bucket != s_bucket)
    {
        s_bucket = bucket;
        s_seen.clear();
    }
    return s_seen.insert(key * 3u + kind).second;
}

// `[G600:disp]`: why the DISPLAY batch did or did not arm G276's pending readback record.
// `[G600:direct]` proves the grab loses its FBO source because `s_g276Pend.active == 0`; this says
// which conjunct of `g276Coalesce` cleared it. Shares DC2_G600_DIRECT so the two land in one run.
static void g600NoteDisplayBatch(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi, bool coalesce,
                                 bool wave261, bool wave272, bool rtt255, bool guestDepth)
{
    if (!g600DirectOn())
        return;
    static const uint32_t every = g599EnvU32("DC2_G600_DIRECT_EVERY", 22u);
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    static uint32_t s_bucket = 0xFFFFFFFFu;
    static uint32_t s_seen = 0u;
    const uint32_t bucket = sf / (every ? every : 1u);
    if (bucket != s_bucket)
    {
        s_bucket = bucket;
        s_seen = 0u;
    }
    const uint32_t bit = 1u << (fbp == 0u ? 0u : 1u);
    if ((s_seen & bit) != 0u)
        return;
    s_seen |= bit;
    std::fprintf(stderr,
                 "[G600:disp] sf=%u fbp=0x%x fbw=%u rows=%d..%d coalesce=%u g261Wave=%u "
                 "g272Wave=%u g255Rtt=%u guestDepth=%u | pend(act=%u fbp=0x%x)\n",
                 sf, fbp, fbw, rowLo, rowHi, coalesce ? 1u : 0u, wave261 ? 1u : 0u,
                 wave272 ? 1u : 0u, rtt255 ? 1u : 0u, guestDepth ? 1u : 0u,
                 s_g276Pend.active ? 1u : 0u, s_g276Pend.fbp);
}

// `[G600:big]`: the texture SOURCE decision, taken at the one point where it is final (the same
// point G599's `[G599:src]` uses, and for the same reason: every bind path `continue`s the moment
// it claims an entry). `src` non-zero = answered from a producer FBO; `view` non-zero = answered
// from a G281/G310 T8 view of a producer FBO; both zero = decoded from guest VRAM below.
static void g600NoteBigEntry(const G144Entry &e, int texW, int texH, uint64_t texKey,
                             uint32_t src, uint64_t view, bool skip)
{
    if (!g600TexOn() || !g600InWindow() || !g600BigTex(e, texW, texH))
        return;
    if (!g600FirstInBucket(texKey, 0u))
        return;
    std::fprintf(stderr,
                 "[G600:big] sf=%u dst=0x%x tbp=0x%x psm=%u tbw=%u cbp=0x%x prim=%u fst=%u "
                 "tex=%dx%d key=%llx skip=%u | src=0x%x view=%llx\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed), e.ctx.frame.fbp,
                 e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.psm), e.ctx.tex0.tbw,
                 e.ctx.tex0.cbp, static_cast<unsigned>(e.prim.type), e.prim.fst ? 1u : 0u,
                 texW, texH, static_cast<unsigned long long>(texKey), skip ? 1u : 0u,
                 src, static_cast<unsigned long long>(view));
}

// `[G600:hit]`: the entry never reached the decode because the texture registry said the GPU copy
// is still current. A STALE cache is the one mechanism that produces a frozen, wrong texture with
// unchanged guest bytes, so it has to be visible as its own event, not as silence.
static void g600NoteTexHit(const G144Entry &e, int texW, int texH, uint64_t texKey,
                           const char *why)
{
    if (!g600TexOn() || !g600InWindow() || !g600BigTex(e, texW, texH))
        return;
    if (!g600FirstInBucket(texKey, 1u))
        return;
    std::fprintf(stderr, "[G600:hit] sf=%u dst=0x%x tbp=0x%x psm=%u tex=%dx%d key=%llx why=%s\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed), e.ctx.frame.fbp,
                 e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.psm), texW, texH,
                 static_cast<unsigned long long>(texKey), why);
}

// `[G600:tex]`: the decoded texels that are about to be uploaded, written as a PPM named for the
// same script key as the -SfDump frame. This is the only artefact in the phase that answers the
// question directly instead of by inference.
static void g600DumpDecoded(const G144Entry &e, int texW, int texH, uint64_t texKey,
                            std::vector<uint32_t> &px)
{
    const uint32_t fill = g600TexFillTbp();
    if (fill != 0u && e.ctx.tex0.tbp0 == fill)
    {
        for (uint32_t &c : px)
            c = 0xFFFF00FFu; // ⛔ solid magenta, opaque: the pixel-space localizer
    }
    else if (g600TexPaintOn())
    {
        const uint32_t col = g600PaintColour(e.ctx.tex0.tbp0);
        for (uint32_t &c : px)
            c = col;
        if (g600FirstInBucket(texKey, 3u))
            std::fprintf(stderr, "[G600:paint] sf=%u dst=0x%x tbp=0x%x psm=%u tex=%dx%d "
                                 "-> %02x%02x%02x\n",
                         g_dc2ScriptFrame.load(std::memory_order_relaxed), e.ctx.frame.fbp,
                         e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.psm), texW, texH,
                         col & 0xFFu, (col >> 8) & 0xFFu, (col >> 16) & 0xFFu);
    }
    if (!g600TexOn() || !g600InWindow() || !g600BigTex(e, texW, texH))
        return;
    if (!g600FirstInBucket(texKey, 2u))
        return;
    const uint32_t key = g600Bucket() * (g600TexEvery() ? g600TexEvery() : 1u);
    const char *tag = std::getenv("DC2_FRAME_DUMP_TAG");
    char path[224];
    std::snprintf(path, sizeof(path),
                  "D:/ps2r/dc2/captures/g600tex_%s_%06u_t%04x_p%02x_%dx%d.ppm",
                  tag != nullptr ? tag : "run", key, e.ctx.tex0.tbp0,
                  static_cast<unsigned>(e.ctx.tex0.psm), texW, texH);
    if (FILE *fp = std::fopen(path, "wb"))
    {
        std::fprintf(fp, "P6\n%d %d\n255\n", texW, texH);
        const size_t n = static_cast<size_t>(texW) * static_cast<size_t>(texH);
        for (size_t k = 0; k < n && k < px.size(); ++k)
        {
            const uint32_t c = px[k];
            std::fputc(static_cast<int>(c & 0xFFu), fp);
            std::fputc(static_cast<int>((c >> 8) & 0xFFu), fp);
            std::fputc(static_cast<int>((c >> 16) & 0xFFu), fp);
        }
        std::fclose(fp);
    }
    std::fprintf(stderr, "[G600:tex] sf=%u key=%u dst=0x%x tbp=0x%x psm=%u tex=%dx%d texKey=%llx\n",
                 g_dc2ScriptFrame.load(std::memory_order_relaxed), key, e.ctx.frame.fbp,
                 e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.psm), texW, texH,
                 static_cast<unsigned long long>(texKey));
}
