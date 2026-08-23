// ===== G495: what is QUEUED AHEAD of each colour readback ======================================
//
// WHY THIS PART EXISTS — and the counter error it corrects.
//
// The G494 roadmap ranked the frame's largest remaining item as "0.707 ms of GPU-pipeline drain +
// 0.476 ms of transfer, PER READBACK, SIX PER FRAME". That reading conflates two different edges.
// `[G418:edge]` is emitted from `renderBatchOnWorker` under `!b.skipReadback` only — the ONE
// render-batch-with-readback the frame contains (fbp 0x13b). Re-measured on the shipped binary:
//
//   [G418:edge] fbp=0x13b(rb=3739 rows=416.0 drain=0.699 xfer=0.499)   over 3781 frames
//
// rb / frames = 0.99. It is **one item per frame**, it lives inside `backend-render` (2.8-3.0 ms/f
// over 34 items), and its 1.20 ms is 40% of that bucket — not of `backend-readcolor`.
//
// The SIX `readColor()` items that `[G447:edge] backend-readcolor=5.65-6.01/6.0` actually times go
// through `readColorOnWorker` -> `g433DoReadPixels`, which has NEVER been split. 5.9 ms/f — 63% of
// the pole thread's blocked half — has no drain/transfer attribution at all. This census supplies it,
// and answers the question the roadmap asked with the wrong subject:
//
//   Is a readback's wait the GPU doing REAL WORK that was queued ahead of it, or a FIXED
//   per-fence pipeline latency that a different fence placement would not pay?
//
// (This is the G491 law — read the code that prints a number before ranking with it — and the G494
// law — a per-item cost that is constant across items of wildly different size is fixed overhead.)
//
// WHAT IT MEASURES (DC2_G495_RB=1, default OFF, behaviour-pure).
//
//   * On the GL worker, every non-fence backend item adds its work to a running "since the last
//     fence" accumulator: item count, draw count, vertex count, texture-upload count and bytes.
//   * A fence is any item that forces the pipeline: a colour readback, a depth readback, or a
//     render batch that reads back. The accumulator resets after each one.
//   * Each colour readback is bracketed with `glFinish()` (drain) and then times the transfer, and
//     files both against the accumulator's contents, keyed by (fbp, rows). glFinish changes no
//     result, no GL state and no command order, so the split is exact; it does move wall time from
//     the transfer half into the drain half, which is the whole point.
//
// EVERY NUMBER IS WINDOWED, never cumulative-over-run divided by frames (the G492 `fallback` trap
// that cost G494 a task): each report prints the delta since the previous report.
//
// DC2_G495_SEQ=1 additionally dumps one contiguous run of the worker's item sequence, once, after
// the queue has warmed — the input for any question about moving a fence inside the stream.
// ==============================================================================================

static bool g495RbCensusOn()
{
    static const bool on = (std::getenv("DC2_G495_RB") != nullptr);
    return on;
}

static bool g495SeqOn()
{
    static const bool on = (std::getenv("DC2_G495_SEQ") != nullptr);
    return on;
}

// Item classes, in the order the worker's dispatch chain tests them.
enum G495Cls
{
    kG495ClsComposite = 0,
    kG495ClsT8 = 1,
    kG495ClsCt24 = 2,
    kG495ClsCopy = 3,
    kG495ClsReadColor = 4,
    kG495ClsWriteColor = 5,
    kG495ClsReadDepth = 6,
    kG495ClsRenderSkip = 7,  // render, skipReadback (leaves work queued)
    kG495ClsRenderRb = 8,    // render + readback (a fence)
    kG495ClsG630Replay = 9,
    kG495ClsG630View = 10,
    kG495ClsG630Upload = 11,
    kG495ClsG630T8 = 12,
    kG495ClsG630Read = 13,
    kG495ClsCount = 14
};

static const char *const kG495ClsName[kG495ClsCount] = {
    "comp", "t8", "ct24", "copy", "rdcol", "wrcol", "rddep", "rend", "rendrb",
    "630replay", "630view", "630upload", "630t8", "630read"};

// GL-worker thread only — single owner, like every other resource in this backend.
struct G495Ahead
{
    uint64_t items = 0, draws = 0, verts = 0, texN = 0, texBytes = 0, fbBytes = 0;
    uint64_t cls[kG495ClsCount] = {};
    void reset() { *this = G495Ahead(); }
};

struct G495Slot
{
    uint64_t n = 0, rows = 0, px = 0, drainNs = 0, xferNs = 0;
    G495Ahead ahead;
    // Previous-report snapshot, so every printed figure is a window delta.
    uint64_t lastN = 0, lastRows = 0, lastPx = 0, lastDrainNs = 0, lastXferNs = 0;
    G495Ahead lastAhead;
};

static G495Ahead g_g495Ahead;
static std::map<uint64_t, G495Slot> g_g495Slots;
static uint64_t g_g495Reads = 0ull;
static uint64_t g_g495SeqDumped = 0ull;

// --- PER-SURFACE readback redundancy ceiling ---------------------------------------------------
// G425 already priced this with a GLOBAL mutation clock — any render/copy/write/composite item at
// all invalidated every cached window — and got 0.0%. That is a strict LOWER bound and it is
// uninformative here, because the frame's item stream never has two readbacks with literally
// nothing between them except the one adjacent pair. The interesting question is per SURFACE: the
// two 0x139/128 reads sit one render apart across the frame boundary, and if that render does not
// target 0x139 the second read returns bytes the first already had.
//
// The clock below ticks only for items whose OUTPUT surface is that fbp:
//   render      -> batch->fbp        (and, when fboKey != 0, the global epoch — the FBO the batch
//                                     actually wrote is then not identified by fbp alone)
//   copy        -> dst colorFbp      writecolor -> colorFbp      composite -> g309OutputFbp
//   t8 / ct24 view -> nothing: those write a texture view object, not a target FBO.
// Both numbers are reported side by side (`redFbp` / `redGlob`) so the strict bound stays visible.
static std::map<uint32_t, uint64_t> g_g495FbpSeq;
static uint64_t g_g495GlobalSeq = 0ull;
static uint64_t g_g495UnknownEpoch = 0ull; // bumped when an item's output surface is unidentified
struct G495RedKey
{
    uint64_t fbpSeq = 0, globalSeq = 0, unknownEpoch = 0;
    bool seen = false;
};
static std::map<uint64_t, G495RedKey> g_g495LastRead;
static uint64_t g_g495RedFbpN = 0, g_g495RedGlobN = 0, g_g495RedFbpNs = 0, g_g495RedGlobNs = 0;
static uint64_t g_g495LastRedFbpN = 0, g_g495LastRedGlobN = 0;
static uint64_t g_g495LastRedFbpNs = 0, g_g495LastRedGlobNs = 0;
static uint64_t g_g495TotN = 0, g_g495TotNs = 0, g_g495LastTotN = 0, g_g495LastTotNs = 0;

// ===== G518: the SUBSUMPTION axis — the one thing G495's clock structurally cannot see ==========
//
// `g_g495LastRead` is keyed on (fbp, glY, rows), so a read is only ever compared against the
// previous read carrying THE SAME WINDOW. On this route the frame's four 0x139 reads are NESTED —
// `@480/32` is inside `@384/128` is inside `@64/448` — so a narrow read that follows a wider read of
// the same surface with no intervening write returns bytes the wider read already had, and the
// key-exact clock reports it as non-redundant because it never compares the two.
//
// This uses the SAME per-fbp write clock above; no per-row write range is needed, because
// containment is a property of the two READ windows and cleanliness is a property of the surface.
// Conservative in the safe direction: any unidentified output surface bumps `g_g495UnknownEpoch`,
// which unproves every covering window, so a false SUBSUMED can only come from a writer that
// `g495NoteWrite` mis-attributes to another fbp — the same trust boundary `redFbp` already has.
//
// `lo/hi` is the union of every read of that surface taken since the last write to it: two reads
// that are individually clean can jointly cover a third.
struct G518Cover
{
    uint64_t fbpSeq = 0, unknownEpoch = 0;
    int lo = 0, hi = -1; // inclusive GL row span; hi < lo means "nothing covered"
    bool seen = false;
};
static std::map<uint32_t, G518Cover> g_g518Cover;
static uint64_t g_g518SubN = 0, g_g518SubNs = 0, g_g518SubRows = 0;
static uint64_t g_g518LastSubN = 0, g_g518LastSubNs = 0, g_g518LastSubRows = 0;
static std::map<uint32_t, uint64_t> g_g518SubByFbp, g_g518LastSubByFbp;

static inline void g495NoteWrite(uint32_t fbp, bool unknown)
{
    if (!g495RbCensusOn())
        return;
    ++g_g495GlobalSeq;
    if (unknown)
        ++g_g495UnknownEpoch; // every surface is now unproven
    else
        ++g_g495FbpSeq[fbp];
}

static inline void g495AheadAdd(G495Ahead &a, const G495Ahead &b)
{
    a.items += b.items;
    a.draws += b.draws;
    a.verts += b.verts;
    a.texN += b.texN;
    a.texBytes += b.texBytes;
    a.fbBytes += b.fbBytes;
    for (int i = 0; i < kG495ClsCount; ++i)
        a.cls[i] += b.cls[i];
}

// Called once per backend item at the worker's dispatch choke point, BEFORE the item runs, so a
// readback never counts itself in its own "ahead" set.
// G644: `fbp` / `glY` / `rows` describe the SURFACE WINDOW the item touches (0 / -1 / 0 when the
// item class has none). Without them the sequence dump names only a class, so a "these two adjacent
// reads overlap" claim is inferred rather than read — which is exactly how G641 §3.2 had to leave
// the containment finding PLAUSIBLE instead of confirmed.
static inline void g495NoteItem(int cls, uint64_t draws, uint64_t verts,
                                uint64_t texN, uint64_t texBytes, uint64_t fbBytes,
                                uint32_t fbp = 0u, int glY = -1, int rows = 0)
{
    if (!g495RbCensusOn())
        return;
    if (g495SeqOn() && g_g495Reads >= 60ull && g_g495SeqDumped < 300ull)
    {
        ++g_g495SeqDumped;
        std::fprintf(stderr,
                     "[G495:seq] %3llu %-6s fbp=0x%x rows=%d..%d draws=%llu verts=%llu "
                     "texN=%llu texKB=%llu\n",
                     static_cast<unsigned long long>(g_g495SeqDumped),
                     (cls >= 0 && cls < kG495ClsCount) ? kG495ClsName[cls] : "?",
                     fbp, glY, rows > 0 ? glY + rows - 1 : glY,
                     static_cast<unsigned long long>(draws),
                     static_cast<unsigned long long>(verts),
                     static_cast<unsigned long long>(texN),
                     static_cast<unsigned long long>(texBytes / 1024ull));
    }
    ++g_g495Ahead.items;
    g_g495Ahead.draws += draws;
    g_g495Ahead.verts += verts;
    g_g495Ahead.texN += texN;
    g_g495Ahead.texBytes += texBytes;
    g_g495Ahead.fbBytes += fbBytes;
    if (cls >= 0 && cls < kG495ClsCount)
        ++g_g495Ahead.cls[cls];
}

// Any pipeline-forcing item clears the accumulator once it has run.
static inline void g495FenceReset()
{
    if (!g495RbCensusOn())
        return;
    g_g495Ahead.reset();
}

static void g495Report()
{
    std::fprintf(stderr, "[G495:rb] win=%llu reads",
                 static_cast<unsigned long long>(g_g495Reads));
    for (auto &kv : g_g495Slots)
    {
        G495Slot &s = kv.second;
        const double n = static_cast<double>(s.n - s.lastN);
        if (n <= 0.0)
            continue;
        const double drainMs = static_cast<double>(s.drainNs - s.lastDrainNs) / 1.0e6 / n;
        const double xferMs = static_cast<double>(s.xferNs - s.lastXferNs) / 1.0e6 / n;
        const double rows = static_cast<double>(s.rows - s.lastRows) / n;
        const double items = static_cast<double>(s.ahead.items - s.lastAhead.items) / n;
        const double draws = static_cast<double>(s.ahead.draws - s.lastAhead.draws) / n;
        const double texKB =
            static_cast<double>(s.ahead.texBytes - s.lastAhead.texBytes) / 1024.0 / n;
        std::fprintf(stderr,
                     " | 0x%x@%d/%d n=%.0f drain=%.3f xfer=%.3f rows=%.0f "
                     "ahead(items=%.1f draws=%.0f texKB=%.0f",
                     static_cast<unsigned>(kv.first >> 20),
                     static_cast<int>((kv.first >> 10) & 0x3ffu),
                     static_cast<int>(kv.first & 0x3ffu),
                     n, drainMs, xferMs, rows, items, draws, texKB);
        for (int i = 0; i < kG495ClsCount; ++i)
        {
            const double c = static_cast<double>(s.ahead.cls[i] - s.lastAhead.cls[i]) / n;
            if (c > 0.0)
                std::fprintf(stderr, " %s=%.1f", kG495ClsName[i], c);
        }
        std::fprintf(stderr, ")");
        s.lastN = s.n;
        s.lastRows = s.rows;
        s.lastPx = s.px;
        s.lastDrainNs = s.drainNs;
        s.lastXferNs = s.xferNs;
        s.lastAhead = s.ahead;
    }
    const double totN = static_cast<double>(g_g495TotN - g_g495LastTotN);
    const double totMs = static_cast<double>(g_g495TotNs - g_g495LastTotNs) / 1.0e6;
    if (totN > 0.0)
        std::fprintf(stderr,
                     " || redFbp=%.1f%%/%.2fms redGlob=%.1f%%/%.2fms of %.0f reads %.1fms",
                     100.0 * static_cast<double>(g_g495RedFbpN - g_g495LastRedFbpN) / totN,
                     static_cast<double>(g_g495RedFbpNs - g_g495LastRedFbpNs) / 1.0e6,
                     100.0 * static_cast<double>(g_g495RedGlobN - g_g495LastRedGlobN) / totN,
                     static_cast<double>(g_g495RedGlobNs - g_g495LastRedGlobNs) / 1.0e6,
                     totN, totMs);
    g_g495LastRedFbpN = g_g495RedFbpN;
    g_g495LastRedGlobN = g_g495RedGlobN;
    g_g495LastRedFbpNs = g_g495RedFbpNs;
    g_g495LastRedGlobNs = g_g495RedGlobNs;
    // G518: the subsumption axis, window-delta like everything else here.
    if (totN > 0.0)
    {
        std::fprintf(stderr, " || SUB=%.1f%%/%.2fms rows=%.0f",
                     100.0 * static_cast<double>(g_g518SubN - g_g518LastSubN) / totN,
                     static_cast<double>(g_g518SubNs - g_g518LastSubNs) / 1.0e6,
                     static_cast<double>(g_g518SubRows - g_g518LastSubRows));
        for (auto &kv : g_g518SubByFbp)
        {
            const uint64_t d = kv.second - g_g518LastSubByFbp[kv.first];
            if (d != 0ull)
                std::fprintf(stderr, " 0x%x=%llu", kv.first,
                             static_cast<unsigned long long>(d));
        }
        g_g518LastSubN = g_g518SubN;
        g_g518LastSubNs = g_g518SubNs;
        g_g518LastSubRows = g_g518SubRows;
        g_g518LastSubByFbp = g_g518SubByFbp;
    }
    g_g495LastTotN = g_g495TotN;
    g_g495LastTotNs = g_g495TotNs;
    std::fprintf(stderr, "\n");
}

// Called from readColorOnWorker with the drain/transfer split already timed.
static inline void g495NoteRead(uint32_t fbp, int glY, int rows, int px,
                                uint64_t drainNs, uint64_t xferNs)
{
    if (!g495RbCensusOn())
        return;
    const uint64_t key = (static_cast<uint64_t>(fbp) << 20) |
                         (static_cast<uint64_t>(glY & 0x3ff) << 10) |
                         static_cast<uint64_t>(rows & 0x3ff);
    G495Slot &s = g_g495Slots[key];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.px += static_cast<uint64_t>(px);
    s.drainNs += drainNs;
    s.xferNs += xferNs;
    g495AheadAdd(s.ahead, g_g495Ahead);
    // Redundancy ceiling, both clocks, priced in the time this read actually cost.
    const uint64_t costNs = drainNs + xferNs;
    ++g_g495TotN;
    g_g495TotNs += costNs;
    G495RedKey &r = g_g495LastRead[key];
    if (r.seen)
    {
        if (r.globalSeq == g_g495GlobalSeq)
        {
            ++g_g495RedGlobN;
            g_g495RedGlobNs += costNs;
        }
        if (r.unknownEpoch == g_g495UnknownEpoch &&
            r.fbpSeq == g_g495FbpSeq[fbp])
        {
            ++g_g495RedFbpN;
            g_g495RedFbpNs += costNs;
        }
    }
    r.seen = true;
    r.globalSeq = g_g495GlobalSeq;
    r.unknownEpoch = g_g495UnknownEpoch;
    r.fbpSeq = g_g495FbpSeq[fbp];
    // G518 subsumption, on the same per-fbp clock.
    {
        G518Cover &c = g_g518Cover[fbp];
        const bool clean = c.seen && c.unknownEpoch == g_g495UnknownEpoch &&
                           c.fbpSeq == g_g495FbpSeq[fbp];
        const int lo = glY, hi = glY + rows - 1;
        if (clean && c.hi >= c.lo && lo >= c.lo && hi <= c.hi)
        {
            ++g_g518SubN;
            g_g518SubNs += costNs;
            g_g518SubRows += static_cast<uint64_t>(rows);
            ++g_g518SubByFbp[fbp];
        }
        if (clean && c.hi >= c.lo)
        {
            c.lo = std::min(c.lo, lo);
            c.hi = std::max(c.hi, hi);
        }
        else
        {
            c.lo = lo;
            c.hi = hi;
        }
        c.seen = true;
        c.unknownEpoch = g_g495UnknownEpoch;
        c.fbpSeq = g_g495FbpSeq[fbp];
    }
    if ((++g_g495Reads % 600ull) == 0ull)
        g495Report();
}

// ===== G495b: how much of the drain is REAL GPU EXECUTION? =====================================
//
// The census above proves a per-fence FLOOR: the two readbacks whose `ahead` set contains nothing
// but themselves (0x0/416 and 0x68/416, i.e. the previous item was itself a fence and the pipeline
// was already empty) still drain 0.31 ms. Everything above that floor is either GPU work that has
// to happen somewhere, or more latency. Only a GPU-side clock can tell those apart, and the whole
// question of whether a fence can usefully be MOVED depends on the answer.
//
// GL_TIME_ELAPSED around every backend item, summed per class, gives GPU busy ns per frame.
// Results are read back with a full ring of lag (256 items ~ 3.5 frames) so the read never waits
// on the GPU. `glGetQueryObjectui64v` DOES have to return a server-side value, so this instrument
// costs ~one G494-class driver rendezvous per item (~74/frame): NEVER quote a frame time from a
// run with DC2_G495_GPU=1. The GPU-side numbers it reports are unaffected by that CPU-side cost.
#ifndef GL495_TIME_ELAPSED
#define GL495_TIME_ELAPSED 0x88BF
#define GL495_QUERY_RESULT 0x8866
#endif

typedef void(APIENTRY *PFN495GENQUERIES)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN495BEGINQUERY)(GLenum, GLuint);
typedef void(APIENTRY *PFN495ENDQUERY)(GLenum);
typedef void(APIENTRY *PFN495GETQUERYOBJECTUI64V)(GLuint, GLenum, uint64_t *);

static bool g495GpuCensusOn()
{
    static const bool on = (std::getenv("DC2_G495_GPU") != nullptr);
    return on;
}

static constexpr int kG495Ring = 256;

struct G495Gpu
{
    PFN495GENQUERIES glGenQueries_ = nullptr;
    PFN495BEGINQUERY glBeginQuery_ = nullptr;
    PFN495ENDQUERY glEndQuery_ = nullptr;
    PFN495GETQUERYOBJECTUI64V glGetQueryObjectui64v_ = nullptr;
    GLuint q[kG495Ring] = {};
    int cls[kG495Ring] = {};
    bool armed[kG495Ring] = {};
    bool tried = false, ok = false;
    int cursor = 0;
    bool active = false;
    uint64_t ns[kG495ClsCount] = {};
    uint64_t n[kG495ClsCount] = {};
    uint64_t lastNs[kG495ClsCount] = {};
    uint64_t lastN[kG495ClsCount] = {};
    uint64_t items = 0, lastReportItems = 0;
};

static G495Gpu g_g495Gpu;

static bool g495GpuReady()
{
    G495Gpu &g = g_g495Gpu;
    if (!g.tried)
    {
        g.tried = true;
        g.ok = loadProc(g.glGenQueries_, "glGenQueries") &&
               loadProc(g.glBeginQuery_, "glBeginQuery") &&
               loadProc(g.glEndQuery_, "glEndQuery") &&
               loadProc(g.glGetQueryObjectui64v_, "glGetQueryObjectui64v");
        if (g.ok)
            g.glGenQueries_(kG495Ring, g.q);
        std::fprintf(stderr, "[G495:gpu] timer queries %s\n", g.ok ? "loaded" : "UNAVAILABLE");
    }
    return g.ok;
}

// Print per-frame GPU busy by class. `frames` is supplied by the caller's item budget: the report
// fires every kG495Ring*8 items and the item rate per frame is the census's own `items` count, so
// the division is done against the measured per-frame item count rather than a guessed one.
static void g495GpuReport(double itemsPerFrame)
{
    G495Gpu &g = g_g495Gpu;
    const double winItems = static_cast<double>(g.items - g.lastReportItems);
    if (winItems <= 0.0 || itemsPerFrame <= 0.0)
        return;
    const double frames = winItems / itemsPerFrame;
    double total = 0.0;
    char line[512];
    int off = std::snprintf(line, sizeof(line), "[G495:gpu] frames=%.0f", frames);
    for (int i = 0; i < kG495ClsCount; ++i)
    {
        const double ms = static_cast<double>(g.ns[i] - g.lastNs[i]) / 1.0e6 / frames;
        const double cnt = static_cast<double>(g.n[i] - g.lastN[i]) / frames;
        total += ms;
        if (cnt > 0.0 && off < static_cast<int>(sizeof(line)) - 1)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.3f/%.1f",
                                 kG495ClsName[i], ms, cnt);
        g.lastNs[i] = g.ns[i];
        g.lastN[i] = g.n[i];
    }
    g.lastReportItems = g.items;
    std::fprintf(stderr, "%s | gpuBusyMs/f=%.3f\n", line, total);
}

static inline void g495GpuBegin(int cls)
{
    if (!g495GpuCensusOn() || !g495GpuReady())
        return;
    G495Gpu &g = g_g495Gpu;
    const int i = g.cursor;
    // Harvest the result this slot still holds before overwriting it. A full ring of lag means the
    // query completed ~3.5 frames ago, so this never waits on the GPU.
    if (g.armed[i])
    {
        uint64_t ns = 0ull;
        g.glGetQueryObjectui64v_(g.q[i], GL495_QUERY_RESULT, &ns);
        const int c = g.cls[i];
        if (c >= 0 && c < kG495ClsCount)
        {
            g.ns[c] += ns;
            ++g.n[c];
        }
        g.armed[i] = false;
    }
    g.glBeginQuery_(GL495_TIME_ELAPSED, g.q[i]);
    g.cls[i] = cls;
    g.active = true;
}

static inline void g495GpuEnd()
{
    G495Gpu &g = g_g495Gpu;
    if (!g.active)
        return;
    g.glEndQuery_(GL495_TIME_ELAPSED);
    g.armed[g.cursor] = true;
    g.active = false;
    g.cursor = (g.cursor + 1) % kG495Ring;
    ++g.items;
    if ((g.items % (kG495Ring * 8)) == 0ull)
        g495GpuReport(74.0); // ~74 backend items per frame on this route (G447: 34+6+11+2+20+1)
}
