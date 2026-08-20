// ===== G621: how many of a readback batch's rows did the batch actually WRITE? =================
//
// WHY. `[G415:stage]` on `dungeon1` prices the colour readback (`rb`) at ~44% of the whole GL
// backend — more than four times the draw loop — and the batches that pay it have a startling
// shape:
//
//   fbp=0x13b/read    n=1205  up=0.097 rb=0.569  rows=416.0 win=416.0 upRows=511.1 draws=1.0
//   fbp=0x13b/read+z  n=2406  up=0.042 rb=0.505  rows=416.0 win=416.0 upRows=192.3 draws=2.0
//
// ONE or TWO draws, a 511-row surface upload, and a 416-row readback. G415 already narrowed the
// read from the full 512 to the CONSUMER's window (416); nobody has ever asked the PRODUCER's
// question: of those 416 rows, how many could this batch have written at all?
//
// A row outside the union of the batch's draws (scissor ∩ vertex Y extent) is untouched FBO
// content. For a batch that reads back, the FBO and guest VRAM agree over the read window at
// batch ENTRY — either `uploadFb` copied VRAM into the FBO, or the front end's gen snapshot
// proved they were already equal — so reading such a row and publishing it into VRAM writes the
// bytes that are already there. It is the identity.
//
// WHAT THIS MEASURES (DC2_G621_SPAN=1, default OFF, behaviour-pure).
//
// Per batch that reaches the colour readback, keyed by (fbp, hasDepth):
//   * `rdRows`   — rows actually read (the shipped narrow window).
//   * `wrSpan`   — rows in the union SPAN of the draws' written extents, clipped to the read
//                  window. A narrowed read must stay ONE contiguous `glReadPixels`, so the SPAN
//                  (hi-lo+1), not the covered-row count, is the payoff denominator.
//   * `wrCover`  — the covered-row count, i.e. what a multi-rect read could reach. Reported so
//                  the gap between "one contiguous read" and "several" is visible rather than
//                  assumed.
//   * `empty`    — batches whose draws write NOTHING inside the read window (pure round trips).
//   * a SPAN HISTOGRAM in eighths of the read window, because a mean over a bimodal population
//     (a full-screen post-process quad vs a small sprite) would name neither mode.
//
// The extent of a draw is bounded two ways and the tighter one wins:
//   scissor  scY0..scY1 (GS top-origin, inclusive) — the hardware clip, exact;
//   geometry floor(min vy) .. ceil(max vy) over the draw's own vertices — conservative in the
//            safe direction (a pixel centre at +0.5 can never land outside the ceil/floor pair).
// Anything unparseable (no draws, degenerate vertex range) counts as FULL span, so the census can
// only ever UNDERSTATE the lever.
// ==============================================================================================

static bool g621SpanCensusOn()
{
    static const bool on = g158EnvFlag("DC2_G621_SPAN");
    return on;
}

static uint64_t g621SpanEvery()
{
    static const uint64_t n = []() -> uint64_t {
        const char *v = std::getenv("DC2_G621_SPAN_EVERY");
        const long long p = v ? std::atoll(v) : 0;
        return p > 0 ? static_cast<uint64_t>(p) : 600ull;
    }();
    return n;
}

struct G621SpanSlot
{
    uint64_t n = 0;
    uint64_t rdRows = 0;   // Σ rows the shipped read actually transfers
    uint64_t wrSpan = 0;   // Σ rows a producer-scoped CONTIGUOUS read would transfer
    uint64_t wrCover = 0;  // Σ rows the draws actually cover (multi-rect floor)
    uint64_t empty = 0;    // batches writing nothing inside the read window
    uint64_t draws = 0;
    uint64_t hist[8] = {}; // wrSpan/rdRows in eighths (index 7 = full span)
};

// GL-worker thread only, like every other resource in this backend.
static std::map<uint32_t, G621SpanSlot> g_g621Slots;
static uint64_t g_g621Batches = 0ull;

// `rdGsLo/rdGsHi` are the read window in GS (top-origin) rows; `fbH` the FBO height.
static void g621NoteReadback(const G178Batch &b, int rdGsLo, int rdGsHi, bool hasDepth)
{
    if (!g621SpanCensusOn() || rdGsHi < rdGsLo)
        return;
    const int rdRows = rdGsHi - rdGsLo + 1;
    int wrLo = 1 << 30, wrHi = -1;
    uint64_t cover = 0;
    bool full = b.draws.empty();
    // Covered-row bitmap over the read window (512 rows max in this backend).
    static std::vector<uint8_t> s_cov;
    s_cov.assign(static_cast<size_t>(rdRows), 0u);
    for (const G178Draw &d : b.draws)
    {
        int lo = static_cast<int>(d.scY0);
        int hi = static_cast<int>(d.scY1);
        if (hi < lo)
        {
            full = true;
            break;
        }
        if (d.vtxCount > 0 && d.firstVtx >= 0 &&
            static_cast<size_t>(d.firstVtx) + static_cast<size_t>(d.vtxCount) <= b.verts.size())
        {
            float vlo = b.verts[static_cast<size_t>(d.firstVtx)].y;
            float vhi = vlo;
            for (int i = 1; i < d.vtxCount; ++i)
            {
                const float y = b.verts[static_cast<size_t>(d.firstVtx) + static_cast<size_t>(i)].y;
                if (y < vlo) vlo = y;
                if (y > vhi) vhi = y;
            }
            const int glo = static_cast<int>(std::floor(vlo));
            const int ghi = static_cast<int>(std::ceil(vhi));
            if (glo > lo) lo = glo;
            if (ghi < hi) hi = ghi;
        }
        else
            full = true;
        if (lo < rdGsLo) lo = rdGsLo;
        if (hi > rdGsHi) hi = rdGsHi;
        if (hi < lo)
            continue; // this draw writes nothing inside the read window
        if (lo < wrLo) wrLo = lo;
        if (hi > wrHi) wrHi = hi;
        for (int y = lo; y <= hi; ++y)
            s_cov[static_cast<size_t>(y - rdGsLo)] = 1u;
    }
    if (full)
    {
        wrLo = rdGsLo;
        wrHi = rdGsHi;
        cover = static_cast<uint64_t>(rdRows);
    }
    else
    {
        for (const uint8_t c : s_cov)
            cover += c;
    }
    const int span = (wrHi >= wrLo) ? (wrHi - wrLo + 1) : 0;
    const uint32_t key = (b.fbp & 0xFFFFu) | (hasDepth ? 0x10000u : 0u);
    G621SpanSlot &s = g_g621Slots[key];
    ++s.n;
    s.rdRows += static_cast<uint64_t>(rdRows);
    s.wrSpan += static_cast<uint64_t>(span);
    s.wrCover += cover;
    s.draws += b.draws.size();
    if (span == 0)
        ++s.empty;
    int bucket = rdRows > 0 ? (span * 8) / rdRows : 7;
    if (bucket > 7) bucket = 7;
    if (bucket < 0) bucket = 0;
    ++s.hist[bucket];

    if ((++g_g621Batches % g621SpanEvery()) != 0ull)
        return;
    std::fprintf(stderr, "[G621:span] total=%llu",
                 static_cast<unsigned long long>(g_g621Batches));
    for (const auto &kv : g_g621Slots)
    {
        const G621SpanSlot &v = kv.second;
        if (v.n == 0)
            continue;
        const double dn = static_cast<double>(v.n);
        std::fprintf(stderr,
                     " fbp=0x%x%s(n=%llu draws=%.1f rdRows=%.1f wrSpan=%.1f wrCover=%.1f "
                     "keep=%.1f%% empty=%llu h=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu)",
                     kv.first & 0xFFFFu, (kv.first & 0x10000u) ? "+z" : "",
                     static_cast<unsigned long long>(v.n),
                     static_cast<double>(v.draws) / dn,
                     static_cast<double>(v.rdRows) / dn,
                     static_cast<double>(v.wrSpan) / dn,
                     static_cast<double>(v.wrCover) / dn,
                     v.rdRows ? 100.0 * static_cast<double>(v.wrSpan) /
                                    static_cast<double>(v.rdRows)
                              : 0.0,
                     static_cast<unsigned long long>(v.empty),
                     static_cast<unsigned long long>(v.hist[0]),
                     static_cast<unsigned long long>(v.hist[1]),
                     static_cast<unsigned long long>(v.hist[2]),
                     static_cast<unsigned long long>(v.hist[3]),
                     static_cast<unsigned long long>(v.hist[4]),
                     static_cast<unsigned long long>(v.hist[5]),
                     static_cast<unsigned long long>(v.hist[6]),
                     static_cast<unsigned long long>(v.hist[7]));
    }
    std::fprintf(stderr, "\n");
}
