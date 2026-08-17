// ===== G605: WHERE THE CPU BAND REPLAY'S CYCLES ACTUALLY GO, BY CLASS x LEAF =====
// revision: 1
//
// WHY THIS EXISTS. G603 proved that on route `s03` "Layer 1" is ~99% CPU band replay and ranked the
// populations by ms: `discov` 22.4 s, `disp` 18.4 s, `legacy` 5.1 s of replay wall in a 120 s run.
// G604 then measured all three against same-run oracles and refuted every attempt to move them to
// the GPU. What is left is to make the replay itself cheaper, and G530 already established that its
// price is CODE SHAPE, not memory and not the algorithm: 1,310 retired cycles for one replayed
// sprite pixel, `cyc/ns = 3.17` (the lane is 100% CPU-occupied, not blocked), ~20 non-inlined calls
// per pixel.
//
// ⛔ BUT `[G530:cyc]` ONLY SPLITS SPRITES, AND ONLY INTO `fast`/`other`. Two things it cannot say
// are exactly the two that choose the lever:
//
//   1. TRIANGLE vs SPRITE. `s03`'s `discov` population is tri/tristrip (G604 §2.3 counted 196,608+
//      admitted entries, "no sprites"), and drawTriangle has never been decomposed this way.
//   2. PER-DRAW vs PER-PIXEL. G580 revision 2 records the trap in its own header: its first
//      leaf-only kernel "left replay cost unchanged because the Ridepod population commits only
//      about 6.4 pixels per replayed triangle" — the cost was the per-DRAW CLUT decode. `[G530:cyc]`
//      itself measures PRO at 0.4% of the `fast` sprite call and 67.3% of the `other` one. A lever
//      aimed at the wrong one of those buys nothing, twice over.
//
// And a structural fact that makes (2) sharp here: `drawTriangle`'s prologue is ~1,900 lines and the
// band replay calls it ONCE PER (ENTRY, BAND) — with the default 8 lanes, eight times per entry.
//
// WHAT IT MEASURES. The same bracket shape `[G530:cyc]` uses (G526's rule: never read a wall lap
// without retired cycles), split at the entry of the raster dispatch, and bucketed by
//
//      g528ClassifyFbp(frame.fbp)   x   { tri-untextured, tri-fast, tri-generic, sprite, line }
//
// so the class ranking `[G529:cls]` produces by ms can be pushed one level down into the leaf that
// spends it. Scope is `t_g144InReplay` only — inline draws are a different population and averaging
// the two is g526_per_item_average_hides_a_bimodal_population.
//
// COST / HYGIENE. Two QueryThreadCycleTime + two steady_clock reads per replayed DRAW (never per
// pixel — that would be the self-inflating census g526_draw_census.inc forbids). Default-off,
// `DC2_G605_CYC=1`, and registered in `tools/run_g477_perf.ps1`'s `$clear` list in the same edit
// that created it (g505_harness_must_restore_what_it_sets). ⚠️ NEVER in a timing run.
//
// ⚠️ G602's law is respected deliberately: this scope holds only integers, and every fprintf lives
// in a `__declspec(noinline)` reporter. A hot function's price can be its PROLOGUE, and MSVC sets
// the callee-saved XMM file from the COLDEST code in the function — a census that printed floats
// inline would tax `drawTriangle` even with the flag off.
// (QueryThreadCycleTime / GetCurrentThread are declared at GLOBAL scope by g526_draw_census.inc,
// which this TU includes far earlier — do not re-declare them inside this namespace, they would be
// a different, internally-linked entity: the G568/G604 linkage lesson.)

namespace
{

enum
{
    kG605LeafTriUntex = 0, // textured==false triangle: no sampler at all
    kG605LeafTriFast,      // g141FastSampleTri admitted the inlined sampler
    kG605LeafTriGeneric,   // fell through to the non-inlined sampleTexture
    kG605LeafSprite,
    kG605LeafLine,
    // G615: the UNTEXTURED sprite. It was previously booked to `sprite` AND — because
    // drawSprite's only markLoop() sits inside `if (gs->m_prim.tme)` — its entire pixel loop was
    // booked as PROLOGUE. That is what `[G605:leaf] disp sprite pro% = 38.8 / 50.79 us/call` was
    // (ROADMAP "Target K"): not a per-draw prologue at all, but a second population's whole fill
    // loop folded into the first population's envelope column. `[G530:cyc]`'s own comment always
    // said "culled/untextured: all of it is prologue"; the G605 copy dropped the word.
    kG605LeafSpriteUntex,
    // G617: the TEXTURED sprite that the G530/G614 span kernel REFUSED. Before this split, one
    // `disp sprite` row averaged an admitted population running the kernel with a refused one
    // walking the generic per-pixel nest, at radically different per-pixel prices — the same
    // "two populations in one bucket" defect G615 found in the `pro%` column, one level down.
    // With `[G615:privz] span(draws=7135)` against `spanRej(samp=102994)` the row was 94% refused
    // BY DRAW and 17% refused BY PIXEL, so neither its `cyc/inside` nor its share named anything.
    kG605LeafSpriteSlow,
    kG605LeafN
};
static const char *const kG605LeafName[kG605LeafN] = {"tri-untex", "tri-fast", "tri-gen",
                                                      "sprite", "line", "spr-untex", "spr-slow"};

// [cls][leaf]; cls is g528ClassifyFbp's value (disp / legacy / discov / other).
static std::atomic<uint64_t> g_g605N[kG528ClsN][kG605LeafN];
static std::atomic<uint64_t> g_g605Px[kG528ClsN][kG605LeafN];      // pixels actually committed
static std::atomic<uint64_t> g_g605Inside[kG528ClsN][kG605LeafN];  // pixels that passed COVERAGE
static std::atomic<uint64_t> g_g605Bbox[kG528ClsN][kG605LeafN];    // band-clipped bbox area
static std::atomic<uint64_t> g_g605ProNs[kG528ClsN][kG605LeafN];
static std::atomic<uint64_t> g_g605LoopNs[kG528ClsN][kG605LeafN];
static std::atomic<uint64_t> g_g605ProCyc[kG528ClsN][kG605LeafN];
static std::atomic<uint64_t> g_g605LoopCyc[kG528ClsN][kG605LeafN];
// G615: calls that returned BEFORE the raster dispatch, so the whole lap landed in the PRO column.
// Without this, `pro%` cannot be read: a high value means either an expensive prologue or a
// population that never reaches a marked loop, and those name completely different levers.
static std::atomic<uint64_t> g_g605Unmarked[kG528ClsN][kG605LeafN];

static bool g605CycOn()
{
    static const bool v = envFlagEnabled("DC2_G605_CYC");
    return v;
}

// ---------------------------------------------------------------------------------------------
// THE SHAPE TABLE — which GS STATE the expensive replayed triangles are in.
//
// Ranking by leaf says "the triangle pixel loop"; it does not say WHICH loop to write. A span
// kernel is admitted by PROPERTY (G526/G527's law), so the properties have to be counted first, and
// counted BY CYCLES rather than by draws (g603_rank_fallback_by_ms_not_entries). This books every
// replayed draw's per-draw-constant GS state into a small fixed table and prints the top shapes by
// lane cycles. Fixed capacity, linear probe, no allocation on the replay path; overflow is booked
// to a single `other` row so the table can never silently under-report.
struct G605Shape
{
    uint64_t key = 0u;
    uint64_t n = 0u, cyc = 0u, px = 0u, inside = 0u;
    // sampled once, from the first draw that created the row
    uint32_t fbp = 0u, tbp = 0u;
    uint64_t alpha = 0u, test = 0u, tex1 = 0u;
    uint32_t fbmsk = 0u;
    uint8_t cls = 0u, prim = 0u, fpsm = 0u, tpsm = 0u, tfx = 0u, tcc = 0u;
    uint8_t zpsm = 0u, ztst = 0u, flags = 0u, lin = 0u;
    // ⛔⛔ G617: `prim` is the PRIMITIVE TYPE, not the leaf, and `tme` was in NEITHER the key nor
    // the print. An UNTEXTURED sprite therefore hashed into the same row as a textured one whenever
    // the stale TEX0 state matched — so on `dungeon1` the ten printed `disp sprite` rows summed to
    // 85.5 M px against a `[G605:leaf] disp sprite` total of 45.2 M, and no row could be attributed
    // to a population. A shape table that cannot separate "samples a texture" from "does not" is
    // not a ranking of kernels.
    uint8_t tme = 0u;
    // G617: the RESOLVED leaf (`kG605LeafName`), so a shape row names the loop that ran it.
    uint8_t leaf = 0u;
};
enum { kG605ShapeCap = 96 };
static G605Shape g_g605Shape[kG605ShapeCap];
static std::mutex g_g605ShapeMtx;
static uint64_t g_g605ShapeOverflowN = 0u, g_g605ShapeOverflowCyc = 0u;

static void g605NoteShape(const G605Shape &s)
{
    std::lock_guard<std::mutex> lk(g_g605ShapeMtx);
    for (int i = 0; i < kG605ShapeCap; ++i)
    {
        G605Shape &row = g_g605Shape[i];
        if (row.n == 0u)
        {
            row = s;
            return;
        }
        if (row.key == s.key)
        {
            row.n += s.n;
            row.cyc += s.cyc;
            row.px += s.px;
            row.inside += s.inside;
            return;
        }
    }
    g_g605ShapeOverflowN += s.n;
    g_g605ShapeOverflowCyc += s.cyc;
}

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g605ShapeReport()
{
    G605Shape copy[kG605ShapeCap];
    uint64_t ovN = 0u, ovCyc = 0u;
    {
        std::lock_guard<std::mutex> lk(g_g605ShapeMtx);
        for (int i = 0; i < kG605ShapeCap; ++i)
            copy[i] = g_g605Shape[i];
        ovN = g_g605ShapeOverflowN;
        ovCyc = g_g605ShapeOverflowCyc;
    }
    int order[kG605ShapeCap];
    int used = 0;
    for (int i = 0; i < kG605ShapeCap; ++i)
        if (copy[i].n != 0u)
            order[used++] = i;
    for (int i = 0; i < used; ++i)
        for (int j = i + 1; j < used; ++j)
            if (copy[order[j]].cyc > copy[order[i]].cyc)
            {
                const int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    uint64_t all = ovCyc;
    for (int i = 0; i < used; ++i)
        all += copy[order[i]].cyc;
    std::fprintf(stderr, "[G605:shape] rows=%d overflow(n=%llu cyc=%llu) totalCyc=%llu\n", used,
                 static_cast<unsigned long long>(ovN),
                 static_cast<unsigned long long>(ovCyc),
                 static_cast<unsigned long long>(all));
    // ⛔ G617: the second column used to be `kPrimName[prim]` — the GS primitive type. It printed
    // "sprite" for both an admitted textured sprite and an untextured fill, which is exactly the
    // distinction a kernel ranking needs. It is now the RESOLVED LEAF.
    for (int i = 0; i < used && i < 16; ++i)
    {
        const G605Shape &r = copy[order[i]];
        std::fprintf(stderr,
            "[G605:shape] #%02d %-6s %-9s cyc%%=%5.2f n=%llu px=%llu inside=%llu cyc/inside=%.1f "
            "tme=%u fbp=%03x fpsm=%x fbmsk=%08x tbp=%04x tpsm=%02x tfx=%u tcc=%u lin=%u tex1=%llx "
            "alpha=%llx test=%llx zpsm=%x ztst=%u iip=%u fst=%u abe=%u fge=%u pabe=%u fba=%u "
            "prz=%u\n",
            i, kG528ClsName[r.cls & 3u], kG605LeafName[r.leaf % kG605LeafN],
            (all > 0u) ? 100.0 * static_cast<double>(r.cyc) / static_cast<double>(all) : 0.0,
            static_cast<unsigned long long>(r.n), static_cast<unsigned long long>(r.px),
            static_cast<unsigned long long>(r.inside),
            (r.inside > 0u) ? static_cast<double>(r.cyc) / static_cast<double>(r.inside) : 0.0,
            r.tme, r.fbp, r.fpsm, r.fbmsk, r.tbp, r.tpsm, r.tfx, r.tcc, r.lin,
            static_cast<unsigned long long>(r.tex1),
            static_cast<unsigned long long>(r.alpha),
            static_cast<unsigned long long>(r.test), r.zpsm, r.ztst,
            (r.flags >> 0) & 1u, (r.flags >> 1) & 1u, (r.flags >> 2) & 1u, (r.flags >> 3) & 1u,
            (r.flags >> 4) & 1u, (r.flags >> 5) & 1u, (r.flags >> 6) & 1u);
    }
    std::fflush(stderr);
}

static uint64_t g605Cycles()
{
    unsigned long long c = 0ull;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return static_cast<uint64_t>(c);
}

#if defined(_MSC_VER)
__declspec(noinline)
#endif
static void g605CycReport()
{
    uint64_t total = 0u;
    for (int c = 0; c < kG528ClsN; ++c)
        for (int l = 0; l < kG605LeafN; ++l)
            total += g_g605N[c][l].load(std::memory_order_relaxed);
    static std::atomic<uint64_t> s_next{200000};
    uint64_t want = s_next.load(std::memory_order_relaxed);
    if (total < want || !s_next.compare_exchange_strong(want, want + 200000u))
        return;

    double allNs = 0.0;
    for (int c = 0; c < kG528ClsN; ++c)
        for (int l = 0; l < kG605LeafN; ++l)
            allNs += static_cast<double>(g_g605ProNs[c][l].load(std::memory_order_relaxed) +
                                         g_g605LoopNs[c][l].load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G605:cyc] total calls=%llu laneMs=%.1f\n",
                 static_cast<unsigned long long>(total), allNs / 1e6);
    for (int c = 0; c < kG528ClsN; ++c)
    {
        for (int l = 0; l < kG605LeafN; ++l)
        {
            const double n = static_cast<double>(g_g605N[c][l].load(std::memory_order_relaxed));
            if (n <= 0.0)
                continue;
            const double px = static_cast<double>(g_g605Px[c][l].load(std::memory_order_relaxed));
            const double ins = static_cast<double>(g_g605Inside[c][l].load(std::memory_order_relaxed));
            const double bb = static_cast<double>(g_g605Bbox[c][l].load(std::memory_order_relaxed));
            const double pns = static_cast<double>(g_g605ProNs[c][l].load(std::memory_order_relaxed));
            const double lns = static_cast<double>(g_g605LoopNs[c][l].load(std::memory_order_relaxed));
            const double pcy = static_cast<double>(g_g605ProCyc[c][l].load(std::memory_order_relaxed));
            const double lcy = static_cast<double>(g_g605LoopCyc[c][l].load(std::memory_order_relaxed));
            const double un = static_cast<double>(
                g_g605Unmarked[c][l].load(std::memory_order_relaxed));
            std::fprintf(stderr,
                "[G605:leaf] %-6s %-9s n=%.0f px=%.0f pxPerCall=%.1f insidePerCall=%.1f "
                "bboxPerCall=%.1f | PRO ms=%.1f us/call=%.2f cyc/call=%.0f | LOOP ms=%.1f "
                "cyc/px=%.1f cyc/inside=%.1f cyc/bbox=%.1f | pro%%=%.1f share%%=%.1f "
                "unmarked=%.0f unmarked%%=%.1f\n",
                kG528ClsName[c], kG605LeafName[l], n, px, px / n, ins / n, bb / n,
                pns / 1e6, pns / n / 1e3, pcy / n,
                lns / 1e6, (px > 0.0) ? lcy / px : 0.0, (ins > 0.0) ? lcy / ins : 0.0,
                (bb > 0.0) ? lcy / bb : 0.0,
                100.0 * pns / (pns + lns + 1e-9),
                (allNs > 0.0) ? 100.0 * (pns + lns) / allNs : 0.0,
                un, 100.0 * un / n);
        }
    }
    std::fflush(stderr);
    g605ShapeReport();
}

// One RAII lap per REPLAYED draw. Inert (one predictable branch) unless DC2_G605_CYC=1.
struct G605CycScope
{
    bool on;
    bool marked;
    bool hasShape;
    int cls;
    int leaf;
    uint64_t px;
    uint64_t inside;
    uint64_t bbox;
    uint64_t c0, cMark;
    G605Shape shape;
    std::chrono::steady_clock::time_point t0, tMark;

    G605CycScope(uint32_t fbp, int leafKind)
        : on(g605CycOn() && t_g144InReplay), marked(false), hasShape(false), cls(kG528ClsOther),
          leaf(leafKind), px(0u), inside(0u), bbox(0u), c0(0u), cMark(0u)
    {
        if (!on)
            return;
        const uint32_t k = g528ClassifyFbp(fbp);
        cls = (k < static_cast<uint32_t>(kG528ClsN)) ? static_cast<int>(k) : kG528ClsOther;
        t0 = std::chrono::steady_clock::now();
        c0 = g605Cycles();
        tMark = t0;
        cMark = c0;
    }
    void setLeaf(int leafKind)
    {
        if (on)
            leaf = leafKind;
    }
    void setPx(uint64_t committed, uint64_t bboxArea)
    {
        if (on)
        {
            px = committed;
            bbox = bboxArea;
        }
    }
    void setInside(uint64_t coveredPx)
    {
        if (on)
            inside = coveredPx;
    }
    // Every argument is a per-DRAW constant. Called once, after the sampler admission is known.
    void setShape(uint32_t fbp, uint8_t prim, uint8_t fpsm, uint32_t fbmsk, uint32_t tbp,
                  uint8_t tpsm, uint8_t tfx, uint8_t tcc, bool lin, uint64_t tex1,
                  uint64_t alpha, uint64_t test, uint8_t zpsm, uint8_t ztst,
                  bool iip, bool fst, bool abe, bool fge, bool pabe, bool fba, bool privZ,
                  bool tme)
    {
        if (!on)
            return;
        hasShape = true;
        shape.cls = static_cast<uint8_t>(cls);
        shape.prim = prim;
        shape.tme = tme ? 1u : 0u;
        shape.fbp = fbp;
        shape.fpsm = fpsm;
        shape.fbmsk = fbmsk;
        shape.tbp = tbp;
        shape.tpsm = tpsm;
        shape.tfx = tfx;
        shape.tcc = tcc;
        shape.lin = lin ? 1u : 0u;
        shape.tex1 = tex1;
        shape.alpha = alpha;
        shape.test = test;
        shape.zpsm = zpsm;
        shape.ztst = ztst;
        shape.flags = static_cast<uint8_t>((iip ? 1u : 0u) | (fst ? 2u : 0u) | (abe ? 4u : 0u) |
                                           (fge ? 8u : 0u) | (pabe ? 16u : 0u) | (fba ? 32u : 0u) |
                                           (privZ ? 64u : 0u));
        // FNV-1a over the fields that select a kernel. `tbp` is deliberately EXCLUDED: a shape is a
        // property, not an address (G526/G527), and including it would explode the table into one
        // row per texture page and hide the ranking this exists to produce.
        uint64_t h = 1469598103934665603ull;
        const uint64_t parts[10] = {
            static_cast<uint64_t>(shape.cls) | (static_cast<uint64_t>(shape.prim) << 8) |
                (static_cast<uint64_t>(shape.fpsm) << 16) |
                (static_cast<uint64_t>(shape.tpsm) << 24) |
                (static_cast<uint64_t>(shape.tfx) << 32) |
                (static_cast<uint64_t>(shape.tcc) << 40) |
                (static_cast<uint64_t>(shape.lin) << 48) |
                (static_cast<uint64_t>(shape.flags) << 56),
            static_cast<uint64_t>(shape.zpsm) | (static_cast<uint64_t>(shape.ztst) << 8) |
                (static_cast<uint64_t>(shape.tme) << 16),
            shape.alpha, shape.test, shape.tex1, shape.fbmsk, shape.fbp,
            0u, 0u, 0u};
        for (int i = 0; i < 10; ++i)
        {
            h ^= parts[i];
            h *= 1099511628211ull;
        }
        shape.key = h;
    }
    // Called at the entry of the raster dispatch: everything before it is the PROLOGUE.
    void markLoop()
    {
        if (!on || marked)
            return;
        marked = true;
        tMark = std::chrono::steady_clock::now();
        cMark = g605Cycles();
        g_g605ProNs[cls][leaf].fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tMark - t0).count()),
            std::memory_order_relaxed);
        g_g605ProCyc[cls][leaf].fetch_add(cMark - c0, std::memory_order_relaxed);
    }
    ~G605CycScope()
    {
        if (!on)
            return;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t c1 = g605Cycles();
        if (!marked)
        {
            // Returned before the raster dispatch (culled, degenerate, scissored out): the whole
            // call is prologue, and that is itself a finding — a lever must know how many replayed
            // draws never rasterize anything. ⛔ G615: it is ONLY that finding if the population
            // really did return early, which is why `unmarked` is now counted beside it. An
            // unmarked call whose loop RAN books that loop here and inflates `pro%` without limit.
            g_g605ProNs[cls][leaf].fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
                std::memory_order_relaxed);
            g_g605ProCyc[cls][leaf].fetch_add(c1 - c0, std::memory_order_relaxed);
            g_g605Unmarked[cls][leaf].fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            g_g605LoopNs[cls][leaf].fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - tMark).count()),
                std::memory_order_relaxed);
            g_g605LoopCyc[cls][leaf].fetch_add(c1 - cMark, std::memory_order_relaxed);
        }
        g_g605N[cls][leaf].fetch_add(1u, std::memory_order_relaxed);
        g_g605Px[cls][leaf].fetch_add(px, std::memory_order_relaxed);
        g_g605Inside[cls][leaf].fetch_add(inside, std::memory_order_relaxed);
        g_g605Bbox[cls][leaf].fetch_add(bbox, std::memory_order_relaxed);
        if (hasShape)
        {
            shape.n = 1u;
            shape.cyc = (c1 - c0);
            shape.px = px;
            shape.inside = inside;
            // G617: the LEAF is only resolved once the draw has chosen its loop, which is after
            // setShape(). Fold it into the row key here so a shape row can never merge two
            // different kernels' populations (and print it as the row's second column).
            shape.leaf = static_cast<uint8_t>(leaf);
            shape.key ^= static_cast<uint64_t>(leaf) * 1099511628211ull;
            g605NoteShape(shape);
        }
        g605CycReport();
    }
};

} // namespace
