// G342 arc#6 slice-1 premise-gate: per-consumer-shape TIME census of the c5 tex-alias
// materialize (kG261MatTexAlias site #1 in rasterizer_vram_materialization.inc). Behavior-pure,
// default-off (DC2_G342_CENSUS=1). Answers the arc#6 question the existing count census (G266)
// and per-cause time (G279) cannot: which RESIDUAL tex-alias CONSUMERS still readback+swizzle,
// ranked by TIME (not count — G338 lesson: rows-per-readback differ 3x), and which are view-able
// (single-source swizzle+CLUT: CT32-alias / CT24 (G338) / T8/T4 (G281)) vs a direct-bindable
// same-layout read the g263 bilinear rule simply misses. Times the g261Materialize call at the
// trigger site and buckets ns+rows+count by the consumer signature. Single flush-owner thread
// contract (as g_g266TexShapes): no lock. Self-contained periodic report (as G340/G341).
static bool g342CensusOn() { static const bool v = envFlagEnabled("DC2_G342_CENSUS"); return v; }

// G342 arc#6 slice-1 BEHAVIOR lever (default-off): actually direct-bind the CT32 same-layout
// bilinear TRISTRIP display consumer to its resident source FBO instead of readback+materialize
// (the "CT32-direct" census class). Reuses the exact g267BilinearTriFootprint tap-window proof +
// g267SamplePagesOverlap dirty-sibling alias screen already present (G267 verified this exact
// prototype bit-exact; it was left census-only because it was perf-NEUTRAL — the readback
// relocated to the source's lifecycle-boundary publication). This re-activates it on the current
// post-G338/G341 stack to MEASURE net (relocate vs remove) + re-confirm correctness. Fail-closed:
// any footprint/alias rejection falls through to the proven materialize.
static bool g342Ct32BindOn() { static const bool v = envFlagEnabled("DC2_G342_CT32_BIND"); return v; }
static std::atomic<uint64_t> g_g342Ct32Binds{0};

struct G342Bucket { uint64_t ns = 0; uint64_t rows = 0; uint64_t n = 0; };
static std::map<uint64_t, G342Bucket> g_g342Shapes; // flush-owner single-thread (as g_g266TexShapes)
static uint64_t g_g342TotalNs = 0, g_g342TotalRows = 0, g_g342TotalN = 0;

static void g342Report(); // periodic, defined below

// PSM (0x00 CT32, 0x01 CT24, 0x02 CT16, 0x13 T8, 0x14 T4, 0x1b T8H, 0x24 T4HL, 0x2c T4HH). The
// view-ability class is what an arc#6 resident shader view would need for THIS consumer:
//  - CT32-direct: tbp==producer base && tbw==producer fbw -> a direct FBO bind (g263) already
//    covers sprites; a bilinear tri/tristrip only misses the current sprite-only bilinear rule.
//  - CT32-alias:  CT32 read at a DIFFERENT tbw/base view of the producer pages -> needs a
//    swizzle-remap shader (the core arc#6 raw-format-FBO deswizzle target).
//  - CT24-G338 / CT24-alias: G338's resident CT24-alpha view (built) covers same-layout.
//  - T8-G281 / T4-G281: a resident swizzle+CLUT shader view (G281 template).
static const char *g342Class(uint32_t psm, uint32_t sameLayout)
{
    switch (psm)
    {
    case 0x00: return sameLayout ? "CT32-direct" : "CT32-alias";
    case 0x01: return sameLayout ? "CT24-G338" : "CT24-alias";
    case 0x02: return "CT16";
    case 0x13: case 0x1b: return "T8-G281";
    case 0x14: case 0x24: case 0x2c: return "T4-G281";
    default: return "other";
    }
}

static uint64_t g342Key(uint32_t dstFbp, uint32_t t, uint32_t psm, uint32_t tbp0, uint32_t tbw,
                        uint32_t prim, uint32_t fst, uint32_t bilin, uint32_t sameLayout,
                        uint32_t isDisplay)
{
    uint64_t k = 0;
    k |= static_cast<uint64_t>(dstFbp & 0x1FFu);        // 0-8
    k |= static_cast<uint64_t>(t & 0x7u) << 9;          // 9-11
    k |= static_cast<uint64_t>(psm & 0x3Fu) << 12;      // 12-17
    k |= static_cast<uint64_t>(prim & 0x7u) << 18;      // 18-20
    k |= static_cast<uint64_t>(fst & 1u) << 21;         // 21
    k |= static_cast<uint64_t>(bilin & 1u) << 22;       // 22
    k |= static_cast<uint64_t>(sameLayout & 1u) << 23;  // 23
    k |= static_cast<uint64_t>(isDisplay & 1u) << 24;   // 24
    k |= static_cast<uint64_t>(tbw & 0x3Fu) << 25;      // 25-30
    k |= static_cast<uint64_t>(tbp0 & 0x3FFFu) << 31;   // 31-44
    return k;
}

// One c5 tex-alias materialize just ran on the CPU for consumer `e` reading resident source `t`
// (producer base tBlock). Attribute its wall time + readback rows to the consumer's shape.
static void g342NoteTexAlias(uint32_t dstFbp, uint32_t t, uint32_t tBlock,
                             const G144Entry &e, const G178EntryState &st,
                             uint32_t rfbw, uint64_t rows, uint64_t ns)
{
    const uint32_t psm = e.ctx.tex0.psm & 0x3Fu;
    const uint32_t tbp0 = e.ctx.tex0.tbp0;
    const uint32_t tbw = e.ctx.tex0.tbw;
    const uint32_t prim = static_cast<uint32_t>(e.prim.type) & 0x7u;
    const uint32_t fst = e.prim.fst ? 1u : 0u;
    const uint32_t bilin = st.bilinear ? 1u : 0u;
    const uint32_t sameLayout = (tbp0 == tBlock && tbw == rfbw) ? 1u : 0u;
    const uint32_t isDisplay = (g248TargetIndex(dstFbp) < 0) ? 1u : 0u;
    const uint64_t k =
        g342Key(dstFbp, t, psm, tbp0, tbw, prim, fst, bilin, sameLayout, isDisplay);
    G342Bucket &b = g_g342Shapes[k];
    b.ns += ns;
    b.rows += rows;
    b.n += 1u;
    g_g342TotalNs += ns;
    g_g342TotalRows += rows;
    g_g342TotalN += 1u;
    if ((g_g342TotalN % 512u) == 0u)
        g342Report();
}

static void g342Report()
{
    if (!g342CensusOn())
        return;
    std::fprintf(stderr,
                 "[G342:texcensus] shapes=%zu totalN=%llu totalMs=%.1f totalRows=%llu "
                 "(c5 tex-alias by consumer, ranked by time)\n",
                 g_g342Shapes.size(), static_cast<unsigned long long>(g_g342TotalN),
                 g_g342TotalNs / 1e6, static_cast<unsigned long long>(g_g342TotalRows));
    std::vector<const std::pair<const uint64_t, G342Bucket> *> order;
    order.reserve(g_g342Shapes.size());
    for (const auto &kv : g_g342Shapes)
        order.push_back(&kv);
    std::sort(order.begin(), order.end(),
              [](const std::pair<const uint64_t, G342Bucket> *a,
                 const std::pair<const uint64_t, G342Bucket> *b) {
                  return a->second.ns > b->second.ns;
              });
    for (const auto *p : order)
    {
        const uint64_t k = p->first;
        const G342Bucket &b = p->second;
        const uint32_t psm = static_cast<uint32_t>((k >> 12) & 0x3Fu);
        const uint32_t sameLayout = static_cast<uint32_t>((k >> 23) & 1u);
        std::fprintf(stderr,
                     "[G342:shape] class=%-11s ms=%8.1f rows=%9llu n=%6llu avgUs=%6.1f | "
                     "dst=%03x src=%03x psm=0x%02x tbp=0x%x tbw=%u prim=%u fst=%u bilin=%u "
                     "same=%u disp=%u\n",
                     g342Class(psm, sameLayout), b.ns / 1e6,
                     static_cast<unsigned long long>(b.rows),
                     static_cast<unsigned long long>(b.n),
                     b.n ? (b.ns / 1e3) / static_cast<double>(b.n) : 0.0,
                     static_cast<unsigned>(k & 0x1FFu), kG261Fbp[(k >> 9) & 0x7u], psm,
                     static_cast<unsigned>((k >> 31) & 0x3FFFu),
                     static_cast<unsigned>((k >> 25) & 0x3Fu),
                     static_cast<unsigned>((k >> 18) & 0x7u),
                     static_cast<unsigned>((k >> 21) & 1u),
                     static_cast<unsigned>((k >> 22) & 1u), sameLayout,
                     static_cast<unsigned>((k >> 24) & 1u));
    }
}
