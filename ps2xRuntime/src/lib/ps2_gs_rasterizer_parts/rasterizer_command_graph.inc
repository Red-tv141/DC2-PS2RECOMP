// ===== G260 NATIVE-RENDERER SLICE 1: frame-scope command graph (OPT-IN, DC2_G260_NR=1) =====
// First executable foundation of the native host renderer (plans/arc-native-renderer.md).
// Today every FBP switch and every non-deferrable primitive DRAINS the single-target deferred
// list (22 mid drains/frame on MAP-0), and RTT-family triangles are structurally excluded from
// deferral entirely (the G247 gap), rasterizing inline scalar on the GS thread. This layer
// replaces "drain at every boundary" with "record across boundaries, execute at real dependency
// edges":
//   - RECORD: draws append to the open per-target batch exactly as G144 does (same G144Entry,
//     same capture-time semantics). A target switch CLOSES the open batch into the frame graph
//     (an O(1) vector move) instead of draining it. RTT-family triangles (all five G248 targets)
//     become recordable, joining the same batches as the G252 RTT sprites.
//   - DEPENDENCY EDGES: each batch carries conservative block-interval read/write sets (color+Z
//     writes, texture+CLUT reads; unknown → overlaps-everything). A VRAM mutation (host upload,
//     local-to-local copy) or VRAM read (local-to-host) executes the pending graph only when its
//     range actually intersects a pending set — the WAR/RAW edge — or is unresolvable. Everything
//     else records through without synchronization.
//   - EXECUTE: batches run strictly in SUBMISSION ORDER through the existing proven flush
//     closures (per-batch whole-flush GPU attempt via g178TryFlushGpu, else the 8-lane band CPU
//     replay). No reordering and no cross-batch merging is ever attempted (that is the refuted
//     G254 shortcut); the graph only DELAYS execution, so the VRAM evolution order observed by
//     every reader is identical to the immediate-drain baseline.
// Backend neutrality: the recorded command unit (state snapshot + vertices + read/write sets +
// executor choice per batch) carries no GL types; the OpenGL 4.6 backend is one executor behind
// g178TryFlushGpu, the banded CPU replay is the other (oracle/fallback). Logical-vs-physical
// resolution separation (pillar 4) keys off the batch target dims (fbW/512 logical); the host
// scale factor multiplies only inside the backend submit — scale=1 in this slice.
// Interactions: requires G144 tile-binning (rides its machinery); declines to engage when the
// G242 GPU-depth or G255/G256 GPU-RTT experiments are active (separate architecture arms).
// Kill: unset DC2_G260_NR (default off). Stats: DC2_G260_STAT=1.
} // namespace — the fwd decl below must be GLOBAL scope to match the later global definition
// fwd (defined later in this file): replayed-batch write notes need it before its definition.
void g178NoteVramWriteRect(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
namespace
{

bool g260NrOn()
{
    static const bool s_on = []() {
        // G261 waves are the GPU executor built ON this substrate — the wave lever implies NR.
        if (!envFlagEnabled("DC2_G260_NR") && !g261WaveOn())
            return false;
        if (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
            return false; // NR records through the G144 capture/replay machinery
        // Keep one architectural variable per arm: the standalone G242/G255/G256 experiments
        // carry their own ownership models and never combine with NR. G261 is the exception by
        // design (it IS the ownership model for GPU-resident RTT on this substrate).
        if (g242GpuDepthOn() || (g255GpuRttOn() && !g261WaveOn()))
            return false;
        return true;
    }();
    return s_on;
}

// G270 (DEFAULT OFF, DC2_G270_LINE_WAVE=1): record the display LINE/LINESTRIP
// border work in the native-renderer graph instead of treating every segment as an inline
// primitive. G268's inclusive dispatch timer attributed ~61.5 ms/frame to A+D XYZ2 draws, while
// G171 showed that the first line in each group was paying the pending graph drain. This lever
// removes that artificial barrier. The GPU executor expands the existing integer Bresenham walk
// into ordered 1x1 quads, preserving the CPU rasterizer's exact pixel coordinates and colors.
// Keep this opt-in until CPU fallback, GPU composition, and downstream presentation all pass.
bool g270LineWaveOn()
{
    static const bool s_on = g260NrOn() && envFlagEnabled("DC2_G270_LINE_WAVE") &&
                             !envFlagEnabled("DC2_G270_NO_LINE_WAVE");
    return s_on;
}

bool g270LineType(uint32_t type)
{
    return type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP;
}

static const bool g_g270Stat = envFlagEnabled("DC2_G270_STAT");
static std::atomic<uint64_t> g_g270Captured{0}, g_g270Pixels{0};

void g270NoteCapture(const GSVertex &v0, const GSVertex &v1)
{
    if (!g_g270Stat)
        return;
    const uint64_t n = g_g270Captured.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const int dx = std::abs(static_cast<int>(v1.x) - static_cast<int>(v0.x));
    const int dy = std::abs(static_cast<int>(v1.y) - static_cast<int>(v0.y));
    g_g270Pixels.fetch_add(static_cast<uint64_t>(std::max(dx, dy)) + 1u,
                           std::memory_order_relaxed);
    if ((n & 0xFFu) == 0u)
        std::fprintf(stderr, "[G270:line] captured=%llu pixels=%llu\n",
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_g270Pixels.load(std::memory_order_relaxed)));
}

// G266: FBW is part of the target identity. MAP-0's fbp=0x139 batch carries 1,059 fbw=2
// character-RTT draws plus ONE trailing fbw=8 display-grab sprite; the width mix is a
// guaranteed whole-flush GPU reject (mixed-target check), sending the whole wave to the scalar
// CPU replay every frame. Split the batch at a width switch exactly like an fbp switch. Engaged
// under NR (graph close, O(1)) and under the G255 verify oracle (legacy drain — so the exact
// CPU-shadow harness can exercise the newly GPU-routed batches). Default path unchanged.
// Kill: DC2_G266_NO_FBW_SPLIT=1.
bool g266FbwSplitOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G266_NO_FBW_SPLIT"))
            return false;
        return g260NrOn() || (g255GpuRttOn() && g255VerifyOn());
    }();
    return s_on;
}

// G286 experiment: admit an ordinary CT32 work target into the frame-scope command graph without
// granting it persistent FBO ownership. The selected 0x13b target is a recurring full-screen
// MAP-0 producer that currently rasterizes inline on the GS thread (~62 ms for its terminal
// sprite). It physically aliases the G261 RTT family, so the GPU attempt must first publish every
// overlapping resident target exactly as the proven CPU replay does. The result is read back into
// VRAM in the same flush and its exact color window advances page generations; downstream
// texture/transfer/presentation consumers therefore keep today's VRAM-authoritative contract.
// G287 PROMOTION (2026-07-18): default-on after fixing the backend's missing texture bind/combine
// state and first-use incomplete sampler, then passing a 210-batch exact oracle plus normal-output
// MAP-0 composition. Kill via DC2_G287_NO_TRANSIENT_TARGET=1 (the G286 kill remains accepted);
// DC2_G286_TRANSIENT_TARGET=1 is retained as a redundant compatibility opt-in.
bool g286TransientTargetOn()
{
    static const bool s_on =
        !envFlagEnabled("DC2_G287_NO_TRANSIENT_TARGET") &&
        !envFlagDisabled("DC2_G287_TRANSIENT_TARGET") &&
        !envFlagEnabled("DC2_G286_NO_TRANSIENT_TARGET") &&
        !envFlagDisabled("DC2_G286_TRANSIENT_TARGET");
    return s_on && g260NrOn();
}

bool g286TransientTarget(uint32_t fbp)
{
    return g286TransientTargetOn() && fbp == 0x13bu;
}

static bool g286StatOn()
{
    static const bool s_on =
        envFlagEnabled("DC2_G287_STAT") || envFlagEnabled("DC2_G286_STAT");
    return s_on;
}

// G289 bring-up: pin the exact consumer-side alias shape before granting any GPU-local-copy
// authority. Diagnostic only; the transfer-side census already proved display CT32 -> dbp=0x2760.
static bool g289CensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G289_CENSUS");
    return s_on;
}

static std::atomic<uint64_t> g_g286Captured{0}, g_g286GpuAttempts{0},
    g_g286GpuSuccess{0}, g_g286RowsPublished{0};

// G272 display-color residency helpers are defined after the G178 framebuffer snapshot state.
// The nested G260 scheduler namespace uses these .cpp-local bridges without a public header.
bool g272DisplayColorWaveOn();
bool g272DirtyInfo(int index, uint32_t &fbp, uint32_t &fbw, G144BlockRange &range);
void g272MaterializeIndex(int index, int cause);
void g272MaterializeAll(int cause);
void g272MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown, int cause);
void g272SetDisplayBatch(uint32_t fbp, uint32_t fbw);

namespace
{
    // Conservative set of disjoint block intervals. Fixed capacity; overflow merges into the
    // nearest interval (over-coverage is safe: it can only force an extra execute, never skip
    // a required one). `unknown` poisons the set → overlaps everything.
    struct G260RangeSet
    {
        G144BlockRange r[8];
        int n = 0;
        bool unknown = false;

        void reset()
        {
            n = 0;
            unknown = false;
        }
        void add(const G144BlockRange &b)
        {
            if (unknown)
                return;
            if (!b.valid)
                return; // empty rect (zero area) — nothing read/written
            // merge into an overlapping/adjacent interval if any
            for (int i = 0; i < n; ++i)
            {
                if (b.first <= r[i].last + 1u && r[i].first <= b.last + 1u)
                {
                    r[i].first = std::min(r[i].first, b.first);
                    r[i].last = std::max(r[i].last, b.last);
                    return;
                }
            }
            if (n < 8)
            {
                r[n++] = b;
                return;
            }
            // capacity: merge into the interval with the smallest resulting span growth
            int best = 0;
            uint64_t bestGrow = ~0ull;
            for (int i = 0; i < n; ++i)
            {
                const uint32_t lo = std::min(r[i].first, b.first);
                const uint32_t hi = std::max(r[i].last, b.last);
                const uint64_t grow = static_cast<uint64_t>(hi - lo) -
                                      (r[i].last - r[i].first);
                if (grow < bestGrow)
                {
                    bestGrow = grow;
                    best = i;
                }
            }
            r[best].first = std::min(r[best].first, b.first);
            r[best].last = std::max(r[best].last, b.last);
        }
        void markUnknown() { unknown = true; }
        bool overlaps(const G144BlockRange &b) const
        {
            if (unknown)
                return true;
            if (!b.valid)
                return false;
            for (int i = 0; i < n; ++i)
                if (g144RangeOverlaps(r[i], b))
                    return true;
            return false;
        }
    };

    struct G260Batch
    {
        uint32_t fbp = 0xFFFFFFFFu;
        std::vector<G144Entry> entries;
        G260RangeSet wr; // color target + active Z (read-modify-write surfaces)
        G260RangeSet rd; // texture + CLUT source pages
    };

    // Before executing a recorded batch, resolve any dirty display FBO that the batch will sample
    // as ordinary VRAM, alias through another target, or reinterpret with a different FBW. A same-
    // layout write to the same display target may continue directly in its persistent FBO.
    void g272PrepareBatch(const G260Batch &b)
    {
        if (!g272DisplayColorWaveOn() || b.entries.empty())
            return;
        const uint32_t nextFbw = b.entries.front().ctx.frame.fbw;
        for (int di = 0; di < 2; ++di)
        {
            uint32_t dirtyFbp = 0u, dirtyFbw = 0u;
            G144BlockRange dirtyRange{};
            if (!g272DirtyInfo(di, dirtyFbp, dirtyFbw, dirtyRange))
                continue;
            bool depthAlias = false;
            for (const G144Entry &e : b.entries)
            {
                const G254Surface depth = g254DepthSurface(e);
                if (depth.active && depth.range.valid &&
                    g144RangeOverlaps(depth.range, dirtyRange))
                {
                    depthAlias = true;
                    break;
                }
            }
            const bool readsDirty = b.rd.overlaps(dirtyRange);
            const bool otherWrite = b.wr.overlaps(dirtyRange) &&
                                    (b.fbp != dirtyFbp || nextFbw != dirtyFbw || depthAlias);
            if (readsDirty || otherWrite)
                g272MaterializeIndex(di, 1); // recorded GPU consumer/alias edge
        }
    }

    std::vector<G260Batch> g_g260Graph;                 // closed batches, submission order
    std::vector<std::vector<G144Entry>> g_g260Pool;     // recycled entry storage
    G260RangeSet g_g260OpenWr;                          // aggregates of the OPEN batch (g_g144List)
    G260RangeSet g_g260OpenRd;
    // stats (quiet unless DC2_G260_STAT=1)
    uint64_t g_g260ExecByCause[8] = {};
    uint64_t g_g260BatchesExecuted = 0;
    uint64_t g_g260EntriesExecuted = 0;
    uint64_t g_g260Closes = 0;
    uint64_t g_g260UploadSkips = 0;
    size_t g_g260MaxGraph = 0;

    // G290 (probe, quiet unless DC2_G290_PROBE=1): drain-execution decomposition. The G273
    // profile times only SUCCESSFUL GPU flushes; this splits the whole per-batch execution —
    // batch preparation, T8 resolve, GPU attempt (success vs reject), pre-replay publication,
    // CPU band replay, and post-replay generation notes — so the unattributed remainder of the
    // g144 upload drain can be located. Accumulated single-threaded on the drain thread.
    bool g290ProbeOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G290_PROBE");
        return s_on;
    }
    uint64_t g_g290PrepNs = 0, g_g290ResolveNs = 0, g_g290GpuOkNs = 0, g_g290GpuFailNs = 0,
             g_g290PubNs = 0, g_g290ReplayNs = 0, g_g290NoteNs = 0;
    uint64_t g_g290GpuOk = 0, g_g290GpuFail = 0, g_g290CpuEntries = 0, g_g290Drains = 0;

    // G271 (DEFAULT OFF, DC2_G271_PREFIX_BARRIER=1): an upload that conflicts with one recorded
    // batch only needs submission-order execution through the LAST conflicting batch. Independent
    // suffix batches may remain recorded across the upload. No reordering is introduced: this is
    // the prefix form of the same G260 range dependency proof, not a new ownership model.
    bool g271PrefixBarrierOn()
    {
        static const bool s_on = g260NrOn() && envFlagEnabled("DC2_G271_PREFIX_BARRIER") &&
                                 !envFlagEnabled("DC2_G271_NO_PREFIX_BARRIER");
        return s_on;
    }
    uint64_t g_g271Execs = 0, g_g271Batches = 0, g_g271SuffixBatches = 0;

    enum G260Cause : int
    {
        kG260Boundary = 0,
        kG260Upload = 1,
        kG260LocalCopy = 2,
        kG260LocalHost = 3,
        kG260NonDef = 4,
        kG260Runaway = 5,
        kG260Legacy = 6,
    };

    bool g260StatOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G260_STAT");
        return s_on;
    }

    // Rect range with fail-closed semantics for the dependency sets: a non-empty rect whose
    // range could not be resolved (block overflow past 0x3FFF etc.) must poison as UNKNOWN,
    // not silently become empty like the raw helper's {} return.
    void g260AddRectOrUnknown(G260RangeSet &set, uint32_t baseBlock, uint8_t psm, uint32_t bw,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (w == 0u || h == 0u)
            return;
        const G144BlockRange rg = g144RangeForRect(baseBlock, psm, bw, x, y, w, h);
        if (!rg.valid)
            set.markUnknown();
        else
            set.add(rg);
    }

    // Accumulate one just-recorded entry's read/write ranges into the open-batch sets.
    void g260NoteAppend(const G144Entry &e)
    {
        // color target (written and, under blending/fbmsk, read back) — scissor-bounded
        const G144BlockRange color = g144TargetRange(e);
        if (!color.valid &&
            e.ctx.scissor.x1 >= e.ctx.scissor.x0 && e.ctx.scissor.y1 >= e.ctx.scissor.y0)
            g_g260OpenWr.markUnknown();
        else
            g_g260OpenWr.add(color);
        // active Z surface (read for ZTST, written unless ZMSK) — treat as write-class
        const G254Surface depth = g254DepthSurface(e);
        if (depth.active)
        {
            if (!depth.range.valid)
                g_g260OpenWr.markUnknown();
            else
                g_g260OpenWr.add(depth.range);
        }
        // texture + CLUT reads
        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
                g_g260OpenRd.markUnknown();
            else
                g_g260OpenRd.add(tex);
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                    g_g260OpenRd.markUnknown();
                else
                    g_g260OpenRd.add(clut);
            }
        }
    }

    bool g260HasPending()
    {
        return !g_g260Graph.empty() || !g_g144List.empty();
    }

    // Close the open batch (g_g144List) into the graph. O(1): vector move, no rasterization.
    void g260CloseOpenBatch()
    {
        if (g_g144List.empty())
            return;
        G260Batch b;
        if (!g_g260Pool.empty())
        {
            b.entries = std::move(g_g260Pool.back());
            g_g260Pool.pop_back();
            b.entries.clear();
        }
        b.entries.swap(g_g144List);
        b.fbp = g_g144BlockFbp;
        b.wr = g_g260OpenWr;
        b.rd = g_g260OpenRd;
        g_g260OpenWr.reset();
        g_g260OpenRd.reset();
        g_g260Graph.push_back(std::move(b));
        ++g_g260Closes;
        if (g_g260Graph.size() > g_g260MaxGraph)
            g_g260MaxGraph = g_g260Graph.size();
    }

    void g260StatReport()
    {
        if (!g260StatOn())
            return;
        static uint64_t s_tick = 0;
        if ((++s_tick % 240ull) != 0ull)
            return;
        std::fprintf(stderr,
                     "[G260:stat] exec(boundary=%llu upload=%llu l2l=%llu l2h=%llu nondef=%llu "
                     "runaway=%llu legacy=%llu) closes=%llu batches=%llu entries=%llu maxGraph=%zu "
                     "uploadSkips=%llu\n",
                     (unsigned long long)g_g260ExecByCause[kG260Boundary],
                     (unsigned long long)g_g260ExecByCause[kG260Upload],
                     (unsigned long long)g_g260ExecByCause[kG260LocalCopy],
                     (unsigned long long)g_g260ExecByCause[kG260LocalHost],
                     (unsigned long long)g_g260ExecByCause[kG260NonDef],
                     (unsigned long long)g_g260ExecByCause[kG260Runaway],
                     (unsigned long long)g_g260ExecByCause[kG260Legacy],
                     (unsigned long long)g_g260Closes,
                     (unsigned long long)g_g260BatchesExecuted,
                     (unsigned long long)g_g260EntriesExecuted,
                     g_g260MaxGraph,
                     (unsigned long long)g_g260UploadSkips);
    }

    // Execute a submission-order prefix of the CLOSED graph. Each batch runs through the SAME
    // proven flush closure the immediate drain uses (whole-flush GPU attempt, else 8-lane band
    // CPU replay). Remaining suffix batches retain their original order and captured state.
    void g260ExecutePrefix(int cause, bool allowParallel, size_t prefixCount)
    {
        if (t_g144InReplay)
            return; // replay lanes never re-enter the scheduler
        prefixCount = std::min(prefixCount, g_g260Graph.size());
        if (prefixCount == 0u)
            return;
        ++g_g260ExecByCause[cause & 7];
        static const bool s_seqEnv = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &fn =
            (allowParallel && !s_seqEnv && g_g144FlushFn) ? g_g144FlushFn : g_g144FlushSeqFn;
        for (size_t bi = 0; bi < prefixCount; ++bi)
        {
            G260Batch &b = g_g260Graph[bi];
            if (b.entries.empty())
                continue;
            g272PrepareBatch(b);
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            ++g_g260BatchesExecuted;
            g_g260EntriesExecuted += g_g144List.size();
            // Ownership rule (the G255 lesson, applied to CPU replay): a replayed batch's color/Z
            // writes must advance the SAME page generations an inline draw would (the inline
            // drawPrimitive-end hook is skipped during replay via t_g144InReplay). Without this,
            // deferred RTT-triangle output freezes its pages' generations and a GPU-classified
            // display batch legally reuses a stale cached texture of those pages — the persistent
            // missing-body-part failure found during G260 bring-up. Bump BEFORE execution order
            // consumers evaluate gens (they only read at their own later execution).
            // G261 waves: this pre-execution bump would force a stale-VRAM uploadFb into a
            // GPU-resident target (VRAM has NOT changed when the batch executes on the GPU).
            // Under waves the equivalent bump happens only on the CPU-replay path, inside the
            // flush closures (g261NoteCpuRttWrites, after materialize-before-CPU-write ran).
            if (g178GpuOn() && !g261WaveOn())
            {
                uint8_t *vram260 = g_g144LastVram; // member-safe alias (set beside g_g144LastGs)
                for (size_t ei = 0; vram260 && ei < g_g144List.size(); ++ei)
                {
                    const G144Entry &e = g_g144List[ei];
                    const GSContext &c = e.ctx;
                    // Scope: RTT-family targets only. In the baseline, RTT triangles rasterized
                    // INLINE and bumped these pages per draw (that inline bump is what kept the
                    // GPU texture cache honest for the composite); deferred DISPLAY draws never
                    // bumped, and replicating a display bump here only churns the fb-snapshot
                    // gens into constant re-uploads (measured as a full perf giveback).
                    if (g248TargetIndex(c.frame.fbp) < 0)
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRect(vram260,
                                              framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRect(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                    }
                }
            }
            if (fn)
                fn(); // drains + clears g_g144List
            else
                g_g144List.clear(); // cannot happen (entries imply assigned closures); fail safe
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g144List.swap(b.entries); // reclaim the (now empty) storage's capacity
            g_g260Pool.push_back(std::move(b.entries));
        }
        g_g260Graph.erase(g_g260Graph.begin(), g_g260Graph.begin() + prefixCount);
        g_g144BlockFbp = 0xFFFFFFFFu;
        g260StatReport();
    }

    // Execute the whole pending graph (closed batches then the open batch) strictly in
    // submission order. allowParallel selects the band-pool closure; callers pass false only
    // from contexts where the pool is not proven (non-pipelined frame boundary on the EE thread).
    void g260ExecuteAll(int cause, bool allowParallel)
    {
        if (t_g144InReplay)
            return;
        g260CloseOpenBatch();
        if (g_g260Graph.empty())
            return;
        ++g_g260ExecByCause[cause & 7];
        static const bool s_seqEnv = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &fn =
            (allowParallel && !s_seqEnv && g_g144FlushFn) ? g_g144FlushFn : g_g144FlushSeqFn;
        // Keep the pre-G271 whole-graph loop on every established/default path. Prefix execution
        // uses its separate opt-in helper only; this avoids changing vector lifetime/capacity or
        // open-batch state for the promoted G260-G266 stack.
        const bool g290Probe = g290ProbeOn();
        for (auto &b : g_g260Graph)
        {
            if (b.entries.empty())
                continue;
            const auto g290Tp0 = g290Probe ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            g272PrepareBatch(b);
            if (g290Probe)
                g_g290PrepNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g290Tp0)
                        .count());
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            ++g_g260BatchesExecuted;
            g_g260EntriesExecuted += g_g144List.size();
            if (g178GpuOn() && !g261WaveOn())
            {
                uint8_t *vram260 = g_g144LastVram;
                for (size_t ei = 0; vram260 && ei < g_g144List.size(); ++ei)
                {
                    const G144Entry &e = g_g144List[ei];
                    const GSContext &c = e.ctx;
                    if (g248TargetIndex(c.frame.fbp) < 0)
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRect(vram260, framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRect(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1));
                    }
                }
            }
            if (fn)
                fn();
            else
                g_g144List.clear();
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g144List.swap(b.entries);
            g_g260Pool.push_back(std::move(b.entries));
        }
        g_g260Graph.clear();
        g_g144BlockFbp = 0xFFFFFFFFu;
        if (g290Probe && (++g_g290Drains % 100u) == 0u)
            std::fprintf(stderr,
                         "[G290:probe] drains=%llu prep=%.1f resolve=%.1f gpuOk=%.1f/%llu "
                         "gpuFail=%.1f/%llu pub=%.1f replay=%.1f/%llue note=%.1f (cumMs)\n",
                         (unsigned long long)g_g290Drains, g_g290PrepNs / 1e6,
                         g_g290ResolveNs / 1e6, g_g290GpuOkNs / 1e6,
                         (unsigned long long)g_g290GpuOk, g_g290GpuFailNs / 1e6,
                         (unsigned long long)g_g290GpuFail, g_g290PubNs / 1e6,
                         g_g290ReplayNs / 1e6, (unsigned long long)g_g290CpuEntries,
                         g_g290NoteNs / 1e6);
        g260StatReport();
    }

    bool g260RangeSetsConflict(const G260RangeSet &wr, const G260RangeSet &rd,
                               const G144BlockRange &src, const G144BlockRange &dst,
                               bool srcUnknown, bool dstUnknown)
    {
        if (srcUnknown || dstUnknown)
            return true;
        if (src.valid && wr.overlaps(src))
            return true;
        return dst.valid && (wr.overlaps(dst) || rd.overlaps(dst));
    }

    // G271: close the open batch, find the last conflicting batch, and execute exactly that
    // prefix. Every earlier batch executes too, preserving submission order; every retained
    // suffix is proven disjoint from the VRAM event's source/destination ranges.
    void g271ExecuteConflictPrefix(const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown, int cause,
                                   bool allowParallel)
    {
        g260CloseOpenBatch();
        if (g_g260Graph.empty())
            return;
        static const bool s_noSkip = envFlagEnabled("DC2_G260_NO_SKIP");
        size_t prefixCount = 0u;
        if (s_noSkip || srcUnknown || dstUnknown)
        {
            prefixCount = g_g260Graph.size();
        }
        else
        {
            for (size_t i = 0; i < g_g260Graph.size(); ++i)
                if (g260RangeSetsConflict(g_g260Graph[i].wr, g_g260Graph[i].rd,
                                          src, dst, false, false))
                    prefixCount = i + 1u;
        }
        if (prefixCount == 0u)
            return; // defensive: the caller's pre-check should have found no edge
        const size_t before = g_g260Graph.size();
        ++g_g271Execs;
        g_g271Batches += prefixCount;
        g_g271SuffixBatches += before - prefixCount;
        g260ExecutePrefix(cause, allowParallel, prefixCount);
        if (g260StatOn() && (g_g271Execs % 120u) == 0u)
            std::fprintf(stderr,
                         "[G271:prefix] exec=%llu batches=%llu retainedSuffix=%llu pending=%zu\n",
                         static_cast<unsigned long long>(g_g271Execs),
                         static_cast<unsigned long long>(g_g271Batches),
                         static_cast<unsigned long long>(g_g271SuffixBatches),
                         g_g260Graph.size());
    }

    // Would a VRAM mutation of [src→]dst intersect any pending recorded work? src matters only
    // for reads of pending WRITE surfaces (a copy must see rasterized pixels); dst conflicts
    // with both pending writes (target alias / WAW+WAR) and pending reads (texture/CLUT WAR).
    bool g260VramEventNeedsExecute(const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown)
    {
        if (!g260HasPending())
            return false;
        // DC2_G260_NO_SKIP=1 (bisect lever): treat every VRAM transfer as a dependency edge —
        // discriminates "range-test false negative" from any other delayed-execution defect.
        static const bool s_noSkip = envFlagEnabled("DC2_G260_NO_SKIP");
        if (s_noSkip)
            return true;
        if (srcUnknown || dstUnknown)
            return true;
        for (const auto &b : g_g260Graph)
            if (g260RangeSetsConflict(b.wr, b.rd, src, dst, false, false))
                return true;
        if (!g_g144List.empty() &&
            g260RangeSetsConflict(g_g260OpenWr, g_g260OpenRd, src, dst, false, false))
            return true;
        return false;
    }
} // namespace

constexpr uint32_t kG178PageCount = 512; // 4MB VRAM / 8KB GS page
std::atomic<uint64_t> g_g178PageGen[kG178PageCount]; // zero-initialized

// page geometry (pixels per GS page) by PSM — used only to compute conservative page RANGES.
void g178PageDims(uint32_t psm, uint32_t &pw, uint32_t &ph)
{
    switch (psm)
    {
    case GS_PSM_T8: pw = 128; ph = 64; break;
    case GS_PSM_T4: pw = 128; ph = 128; break;
    case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
        pw = 64; ph = 64; break;
    default: pw = 64; ph = 32; break; // CT32/CT24/Z32/Z24
    }
}

// Sum of write-generations over a conservative page range for a rect at BLOCK base `bp`.
// Any VRAM write into the range changes the sum (each bump is a monotonic increment).
uint64_t g178GenSumRect(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t pw, ph;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t py0 = y / ph, py1 = (y + std::max(1u, h) - 1u) / ph;
    const uint32_t px0 = x / pw, px1 = (x + std::max(1u, w) - 1u) / pw;
    const uint32_t basePage = bpBlocks >> 5;
    uint64_t sum = 0;
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1; ++px)
        {
            // +1 slack page for non-page-aligned block bases (over-coverage is safe: it can only
            // cause a spurious re-decode, never a stale texture).
            const uint32_t pg = basePage + py * widthPages + px;
            sum += g_g178PageGen[pg & (kG178PageCount - 1)].load(std::memory_order_relaxed);
            sum += g_g178PageGen[(pg + 1) & (kG178PageCount - 1)].load(std::memory_order_relaxed);
        }
    return sum;
}

// G277-R: slack-free gen sum for a PAGE-ALIGNED CT32 window (G273's measured display tuple).
// g178GenSumRect deliberately reads each page's +1 slack neighbour (safe for texture re-decode,
// where over-coverage only costs a spurious decode). For the G276 display fb snapshot that slack
// is actively harmful: the rows 0..415 window's last page (207) reads page 208 = zbp 0xd0's first
// Z page, so every display Z-wave readback bump spuriously invalidated the snapshot — and with
// G276's deferred readback the forced stale-VRAM re-upload erased the unpublished FBO rows (the
// MAP-0 Max-body flicker). A page-aligned base with exact-multiple dims tiles exactly; no slack
// is needed for correctness here because the snapshot only reasons about rows it covers.
static uint64_t g277GenSumAligned(uint32_t fbBlock, uint32_t fbw, uint32_t rows)
{
    const uint32_t basePage = fbBlock >> 5;               // caller guarantees (fbBlock & 31) == 0
    const uint32_t widthPages = std::max(1u, fbw);        // CT32 page = 64 px wide
    const uint32_t pageRows = rows / 32u;                 // caller guarantees rows % 32 == 0
    uint64_t sum = 0;
    for (uint32_t pr = 0; pr < pageRows; ++pr)
        for (uint32_t pc = 0; pc < widthPages; ++pc)
            sum += g_g178PageGen[(basePage + pr * widthPages + pc) & (kG178PageCount - 1)]
                       .load(std::memory_order_relaxed);
    return sum;
}

// G276 pending-display state (declared before g178BumpRectImpl so the G277 mover probe can read
// it; the lever/flush logic lives later with the other G276 code).
struct G276PendDisplay
{
    bool active = false;
    uint32_t fbp = 0u, fbw = 0u, fbBlock = 0u;
    int fbW = 0;
    int rowLo = 1 << 30, rowHi = -1;
};
static G276PendDisplay s_g276Pend;

void g178BumpRectImpl(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // G272 diagnostic: identify every generation publisher that touches a display range while its
    // FBO is ahead of VRAM. This is required to distinguish conservative page-range noise from an
    // actual escaped writer before the display residency arm can ever be promoted.
    static const bool s_g272Stat = envFlagEnabled("DC2_G272_STAT");
    if (s_g272Stat && g272DisplayColorWaveOn())
    {
        const G144BlockRange bump = g144RangeForRect(bpBlocks, static_cast<uint8_t>(psm), bw64,
                                                     x, y, w, h);
        for (int di = 0; di < 2; ++di)
        {
            uint32_t dirtyFbp = 0u, dirtyFbw = 0u;
            G144BlockRange dirty{};
            if (bump.valid && g272DirtyInfo(di, dirtyFbp, dirtyFbw, dirty) &&
                g144RangeOverlaps(bump, dirty))
            {
                static std::atomic<uint32_t> s_n{0};
                const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
                if (n < 160u)
                    std::fprintf(stderr,
                                 "[G272:bump] n=%u dirtyFbp=0x%x bp=0x%x bw=%u psm=0x%x rect=(%u,%u %ux%u)\n",
                                 n, dirtyFbp, bpBlocks, bw64, psm, x, y, w, h);
            }
        }
    }
    uint32_t pw, ph;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t py0 = y / ph, py1 = (y + std::max(1u, h) - 1u) / ph;
    const uint32_t px0 = x / pw, px1 = (x + std::max(1u, w) - 1u) / pw;
    const uint32_t basePage = bpBlocks >> 5;
    // G277-R mover probe: report every bump whose page walk (incl. the +1 slack) can touch the
    // 0x68 display chunk-12 pages 200..207 — pins the writer that trips publish-before-upload.
    {
        static const bool s_g277gm = envFlagEnabled("DC2_G277_GENMOVE");
        if (s_g277gm)
        {
            const uint32_t pgLo = basePage + py0 * widthPages + px0;
            const uint32_t pgHi = basePage + py1 * widthPages + px1 + 1u; // +1 slack
            if (pgLo <= 207u && pgHi >= 200u)
            {
                // Split the census: the frequent full-scissor inline-hook bump (pgLo=104) vs any
                // NARROW bump (pgLo>=160) — only bumps landing while the 0x68 pending union is
                // armed can trip publish-before-upload.
                static std::atomic<uint32_t> s_nFull{0}, s_nNarrow{0};
                const bool narrow = pgLo >= 160u;
                const uint32_t n = narrow
                    ? s_nNarrow.fetch_add(1u, std::memory_order_relaxed)
                    : s_nFull.fetch_add(1u, std::memory_order_relaxed);
                if (n < (narrow ? 48u : 8u))
                    std::fprintf(stderr,
                                 "[G277:bump200] %s n=%u pend=%d bp=0x%x bw=%u psm=0x%x rect=(%u,%u %ux%u) pages=%u..%u\n",
                                 narrow ? "NARROW" : "full", n,
                                 (s_g276Pend.active && s_g276Pend.fbp == 0x68u) ? 1 : 0,
                                 bpBlocks, bw64, psm, x, y, w, h, pgLo, pgHi);
            }
        }
    }
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1; ++px)
        {
            const uint32_t pg = basePage + py * widthPages + px;
            g_g178PageGen[pg & (kG178PageCount - 1)].fetch_add(1u, std::memory_order_relaxed);
            g_g178PageGen[(pg + 1) & (kG178PageCount - 1)].fetch_add(1u, std::memory_order_relaxed);
        }
}

// FNV-1a over the full texture descriptor → the backend's cache key. Bit 63 forced so 0 stays
// reserved for "untextured".
uint64_t g178TexKey(const GSContext &ctx, const GSTexaReg &texa,
                    const GSTexClutReg &texclut)
{
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { hsh ^= v; hsh *= 1099511628211ull; };
    mix(ctx.tex0.tbp0);
    mix((static_cast<uint64_t>(ctx.tex0.tbw) << 32) | ctx.tex0.cbp);
    mix((static_cast<uint64_t>(ctx.tex0.tw) << 40) | (static_cast<uint64_t>(ctx.tex0.th) << 32) |
        (static_cast<uint64_t>(ctx.tex0.psm) << 24) | (static_cast<uint64_t>(ctx.tex0.cpsm) << 16) |
        (static_cast<uint64_t>(ctx.tex0.csm) << 8) | ctx.tex0.csa);
    mix((static_cast<uint64_t>(texa.aem ? 1u : 0u) << 16) |
        (static_cast<uint64_t>(texa.ta1) << 8) | texa.ta0);
    // lookupCLUT addresses CBP through the captured TEXCLUT window.  DC2's RTT effect atlases
    // deliberately reuse one TBP/CBP descriptor with different CBW/COU/COV windows; omitting the
    // window aliases those distinct decoded textures to one backend cache entry.
    mix((static_cast<uint64_t>(texclut.cbw) << 32) |
        (static_cast<uint64_t>(texclut.cou) << 16) | texclut.cov);
    return hsh | (1ull << 63);
}

// Front-end registry entry: what the backend has resident, the page-gen sum its decode saw, and
// a CONTENT hash of the source pages — DC2 re-uploads texture data every frame ("transfer is
// cheaper than space"), so gen-only invalidation would re-decode everything every frame; the
// hash detects byte-identical re-uploads and keeps the GPU copy resident.
struct G178TexReg
{
    uint64_t genSum = 0;
    uint64_t contentHash = 0;
    int w = 0, h = 0;
};
std::unordered_map<uint64_t, G178TexReg> g_g178TexReg;

// Collect the distinct VRAM page indices a texture's conservative range covers (same walk as
// g178GenSumRect, deduped), then FNV-hash those pages' raw bytes. ~8-32 pages for title textures
// — a sequential ~64-256KB hash, far cheaper than a swizzle+CLUT whole-texture decode.
uint64_t g178HashPages(const uint8_t *vram, uint32_t vramSize,
                       uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                       uint32_t w, uint32_t h,
                       uint32_t clutBpBlocks)
{
    bool mark[kG178PageCount] = {};
    auto collect = [&](uint32_t bp, uint32_t bw, uint32_t p, uint32_t ww, uint32_t hh) {
        uint32_t pw, ph;
        g178PageDims(p, pw, ph);
        const uint32_t widthPages = std::max(1u, (std::max(1u, bw) * 64u) / pw);
        const uint32_t py1 = (std::max(1u, hh) - 1u) / ph;
        const uint32_t px1 = (std::max(1u, ww) - 1u) / pw;
        const uint32_t basePage = bp >> 5;
        for (uint32_t py = 0; py <= py1; ++py)
            for (uint32_t px = 0; px <= px1; ++px)
            {
                const uint32_t pg = basePage + py * widthPages + px;
                mark[pg & (kG178PageCount - 1)] = true;
                mark[(pg + 1) & (kG178PageCount - 1)] = true;
            }
    };
    collect(bpBlocks, bw64, psm, w, h);
    collect(clutBpBlocks, 1, GS_PSM_CT32, 64, 32);

    uint64_t hsh = 1469598103934665603ull;
    for (uint32_t pg = 0; pg < kG178PageCount; ++pg)
    {
        if (!mark[pg])
            continue;
        const size_t off = static_cast<size_t>(pg) * 8192u;
        if (off + 8192u > vramSize)
            continue;
        const uint8_t *p = vram + off;
        for (size_t i = 0; i < 8192u; i += 8)
        {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            hsh ^= v;
            hsh *= 1099511628211ull;
        }
    }
    return hsh;
}

// Per-fbp framebuffer snapshot state: when the fb pages' gen sum is unchanged since our last
// readback (and the row window is inside what we have ever uploaded/read back), the FBO already
// equals VRAM and the upload can be skipped.
struct G178FbSnap
{
    uint64_t genSum = 0;
    int rowLo = 1 << 30, rowHi = -1; // rows ever covered by an upload/readback
    bool valid = false;
};
std::map<uint32_t, G178FbSnap> g_g178FbSnap;

// G272 (DEFAULT OFF, DC2_G272_DISPLAY_COLOR_WAVE=1): extend the native renderer's existing
// residency contract to the two CT32 display targets. Unlike the RTT-only G261 records, these
// entries are materialized at presentation as well as at transfer/CPU/texture dependency edges.
// This removes synchronous full-FBO readback from intermediate display batches while keeping
// guest VRAM authoritative whenever normal downstream presentation or a CPU reader needs it.
struct G272DisplayRes
{
    uint32_t fbp = 0u;
    uint32_t fbw = 0u;
    int fbW = 0;
    bool dirty = false;
    int dirtyLo = 512;
    int dirtyHi = -1;
};
G272DisplayRes g_g272Display[2] = {{0x0u}, {0x68u}};
uint32_t g_g272VramSize = 0u;
thread_local bool t_g272DisplayBatch = false;
static std::atomic<uint64_t> g_g272Waves{0}, g_g272Materializes{0}, g_g272Failures{0},
    g_g272GenMoves{0};

bool g272DisplayColorWaveOn()
{
    static const bool s_on = g260NrOn() && g261WaveOn() &&
                             envFlagEnabled("DC2_G272_DISPLAY_COLOR_WAVE") &&
                             !envFlagEnabled("DC2_G272_NO_DISPLAY_COLOR_WAVE");
    return s_on;
}

static int g272DisplayIndex(uint32_t fbp)
{
    return fbp == 0x0u ? 0 : (fbp == 0x68u ? 1 : -1);
}

void g272SetDisplayBatch(uint32_t fbp, uint32_t fbw)
{
    t_g272DisplayBatch = g272DisplayColorWaveOn() && g272DisplayIndex(fbp) >= 0;
    if (!t_g272DisplayBatch)
        return;
    G272DisplayRes &r = g_g272Display[g272DisplayIndex(fbp)];
    if (r.dirty && r.fbw != fbw)
        g272MaterializeIndex(g272DisplayIndex(fbp), 2); // layout change before FBO recreation
}

bool g272DirtyInfo(int index, uint32_t &fbp, uint32_t &fbw, G144BlockRange &range)
{
    if (index < 0 || index >= 2 || !g_g272Display[index].dirty)
        return false;
    const G272DisplayRes &r = g_g272Display[index];
    fbp = r.fbp;
    fbw = r.fbw;
    range = g144RangeForRect(framePageBaseToBlock(r.fbp), GS_PSM_CT32, r.fbw,
                             0u, static_cast<uint32_t>(r.dirtyLo),
                             static_cast<uint32_t>(r.fbW),
                             static_cast<uint32_t>(r.dirtyHi - r.dirtyLo + 1));
    return range.valid;
}

static void g272MarkDirty(uint32_t fbp, uint32_t fbw, int fbW, int rowLo, int rowHi,
                          uint32_t vramSize)
{
    const int index = g272DisplayIndex(fbp);
    if (index < 0)
        return;
    G272DisplayRes &r = g_g272Display[index];
    r.fbw = fbw;
    r.fbW = fbW;
    r.dirty = true;
    r.dirtyLo = std::min(r.dirtyLo, rowLo);
    r.dirtyHi = std::max(r.dirtyHi, rowHi);
    g_g272VramSize = vramSize;
    g_g272Waves.fetch_add(1u, std::memory_order_relaxed);
}

void g272MaterializeIndex(int index, int cause)
{
    if (index < 0 || index >= 2)
        return;
    G272DisplayRes &r = g_g272Display[index];
    if (!r.dirty)
        return;
    const int rows = r.dirtyHi - r.dirtyLo + 1;
    const int glY = 512 - 1 - r.dirtyHi;
    uint8_t *vram = g_g144LastVram;
    static std::vector<uint32_t> s_px;
    const bool ok = vram != nullptr && g_g272VramSize != 0u && r.fbW > 0 && rows > 0 &&
                    g178_backend_read_color(r.fbp, r.fbW, 512, glY, rows, s_px) &&
                    s_px.size() == static_cast<size_t>(r.fbW) * rows;
    if (ok)
    {
        const uint32_t fbBlock = framePageBaseToBlock(r.fbp);
        for (int y = r.dirtyLo; y <= r.dirtyHi; ++y)
        {
            const size_t srcRow = static_cast<size_t>(511 - y - glY) * r.fbW;
            for (int x = 0; x < r.fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(
                    fbBlock, r.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                if (off + 4u <= g_g272VramSize)
                    std::memcpy(vram + off, &s_px[srcRow + x], 4u);
            }
        }
        g178BumpRectImpl(fbBlock, r.fbw, GS_PSM_CT32, 0u,
                         static_cast<uint32_t>(r.dirtyLo), static_cast<uint32_t>(r.fbW),
                         static_cast<uint32_t>(rows));
        g149BumpVramGen();
        G178FbSnap &snap = g_g178FbSnap[r.fbp];
        snap.valid = true;
        snap.genSum = g178GenSumRect(fbBlock, r.fbw, GS_PSM_CT32, 0u, 0u,
                                     static_cast<uint32_t>(r.fbW), 512u);
        g_g272Materializes.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        std::fprintf(stderr,
                     "[G272:materialize-fail] fbp=0x%x rows=%d..%d fbW=%d cause=%d\n",
                     r.fbp, r.dirtyLo, r.dirtyHi, r.fbW, cause);
        G178FbSnap &snap = g_g178FbSnap[r.fbp];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
        g_g272Failures.fetch_add(1u, std::memory_order_relaxed);
    }
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    static const bool s_stat = envFlagEnabled("DC2_G272_STAT");
    const uint64_t mats = g_g272Materializes.load(std::memory_order_relaxed);
    if (s_stat && mats != 0u && (mats % 120u) == 0u)
        std::fprintf(stderr, "[G272:stat] waves=%llu materialize=%llu fail=%llu genMoves=%llu\n",
                     static_cast<unsigned long long>(g_g272Waves.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(mats),
                     static_cast<unsigned long long>(g_g272Failures.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g272GenMoves.load(std::memory_order_relaxed)));
}

void g272MaterializeAll(int cause)
{
    if (!g272DisplayColorWaveOn())
        return;
    g272MaterializeIndex(0, cause);
    g272MaterializeIndex(1, cause);
}

void g272MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                              bool srcUnknown, bool dstUnknown, int cause)
{
    if (!g272DisplayColorWaveOn())
        return;
    if (srcUnknown || dstUnknown)
    {
        g272MaterializeAll(cause);
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        uint32_t fbp = 0u, fbw = 0u;
        G144BlockRange range{};
        if (g272DirtyInfo(i, fbp, fbw, range) &&
            ((src.valid && g144RangeOverlaps(range, src)) ||
             (dst.valid && g144RangeOverlaps(range, dst))))
            g272MaterializeIndex(i, cause);
    }
}

// G178 stats ([G178:stat] every 300 flushes when enabled).
std::atomic<uint64_t> g_g178Flushes{0}, g_g178GpuFlushes{0}, g_g178Fallbacks{0};
std::atomic<uint64_t> g_g178TexDecodes{0}, g_g178TexHits{0}, g_g178TexHashHits{0};
std::atomic<uint64_t> g_g178FbUploads{0}, g_g178FbUploadSkips{0};
std::atomic<uint64_t> g_g178RejectState{0}, g_g178RejectBlend{0}, g_g178RejectTex{0};

// GSRasterizer::lookupCLUT reads the CLUT WINDOW (cbw/cou/cov) from `gs->m_texclut` and the
// alpha-key from `gs->m_texa` -- both LIVE state, correct for CPU inline/replay callers (which
// install the entry's own snapshot as active state first) but WRONG for g178DecodeWhole, which
// runs at flush time against whatever the game's live CLUT window has moved on to since this
// entry was captured (e.g. a later glyph's differently-positioned CLUT slot inside a shared CLUT
// bank). Same bug class as the T4HH nibble-plane fix above, second instance: replicate
// lookupCLUT's logic here using the entry's OWN captured `texclut`/`texa`.
uint32_t g178LookupClutFixed(uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut,
                             uint8_t index, uint32_t cbp, uint8_t cpsm, uint8_t csm, uint8_t csa,
                             uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, csm, csa, sourcePsm);
    const uint32_t clutWidth = (texclut.cbw != 0u) ? static_cast<uint32_t>(texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(texclut.cov) + (clutIndex >> 4);
    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(texa, cpsm, GSMem::ReadCT32(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(texa, cpsm, GSMem::ReadCT24(vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16(vram, cbp, clutWidth, clutX, clutY))));
    case GS_PSM_CT16S:
        return applyTexa(texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16S(vram, cbp, clutWidth, clutX, clutY))));
    default:
        break;
    }
    return 0xFFFF00FFu;
}

// Eager whole-texture decode — the SAME leaf expressions as g149BuildDecoded's eager loop
// (readTexel*/ReadP8 → lookupCLUT), so GPU sampling sees byte-identical texels to the CPU path.
void g178DecodeWhole(GSRasterizer *self, GS *gs, uint8_t *vram, const GSContext &ctx,
                     const GSTexaReg &texa, const GSTexClutReg &texclut, int texW, int texH,
                     uint32_t *outPx)
{
    const uint8_t psm = ctx.tex0.psm;
    const uint32_t pageBwT8 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
    const auto decodeRows = [&](int rowLo, int rowHi, uint32_t *dst) {
        for (int vv = rowLo; vv <= rowHi; ++vv)
            for (int uu = 0; uu < texW; ++uu)
        {
            uint32_t c;
            switch (psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_CT24:
                c = applyTexa(texa, psm, self->readTexelPSMCT32(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
                c = applyTexa(texa, psm, self->readTexelPSMCT16(gs, ctx.tex0.tbp0, ctx.tex0.tbw, uu, vv));
                break;
            case GS_PSM_T8:
            {
                const uint8_t idx = static_cast<uint8_t>(GSMem::ReadP8(vram, ctx.tex0.tbp0, pageBwT8,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv)));
                c = g178LookupClutFixed(vram, texa, texclut, idx, ctx.tex0.cbp, ctx.tex0.cpsm,
                                        ctx.tex0.csm, ctx.tex0.csa, psm);
                break;
            }
            default: // T4 / T4HL / T4HH
            {
                // readTexelPSMT4 derives the HH/HL/plain split from gs->activeContext().tex0.psm
                // (the LIVE GS state) -- correct for the CPU inline/replay paths (which install the
                // entry's ctx as active before sampling) but WRONG here: g178DecodeWhole runs at
                // flush time against whatever the game's active tex0.psm has moved on to since this
                // entry was captured, silently reading the wrong nibble plane for T4HH/T4HL sources
                // (e.g. the costume value-bar text atlas, tbp0=0x1a00 psm=T4HH per G5). Dispatch on
                // this entry's OWN captured `psm` instead, calling GSMem's readers directly.
                uint32_t idx;
                if (psm == GS_PSM_T4HH)
                    idx = GSMem::ReadP4HH(vram, ctx.tex0.tbp0, ctx.tex0.tbw,
                                          static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                else if (psm == GS_PSM_T4HL)
                    idx = GSMem::ReadP4HL(vram, ctx.tex0.tbp0, ctx.tex0.tbw,
                                          static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                else
                {
                    const uint32_t pageBwT4 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
                    idx = GSMem::ReadP4(vram, ctx.tex0.tbp0, pageBwT4,
                                        static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
                }
                c = g178LookupClutFixed(vram, texa, texclut, static_cast<uint8_t>(idx), ctx.tex0.cbp,
                                        ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, psm);
                break;
            }
            }
            dst[static_cast<size_t>(vv) * static_cast<size_t>(texW) +
                static_cast<size_t>(uu)] = c;
        }
    };

    // G298 promotion (default on): whole-texture decode is a read-only VRAM/CLUT transform
    // into independent RGBA rows. Reuse the joined row pool and complete every row before the
    // upload is appended to the backend batch. Keep combined and stage-local kill switches.
    static const bool s_g298ParTexDecode =
        !envFlagEnabled("DC2_G298_NO_PAR_PREP") &&
        !envFlagEnabled("DC2_G298_NO_PAR_TEXDECODE") &&
        !envFlagDisabled("DC2_G298_PAR_TEXDECODE");
    static const int s_g298TexDecodeLanes = [] {
        const char *value = std::getenv("DC2_G298_TEXDECODE_LANES");
        if (value == nullptr)
            return 8;
        return std::max(1, std::min(16, std::atoi(value)));
    }();
    const int pixels = texW * texH;
    const int lanes =
        s_g298ParTexDecode && texH >= 8 && pixels >= 4096
            ? s_g298TexDecodeLanes
            : 1;
    const auto joinedDecode = [&](int rowLo, int rowHi) {
        decodeRows(rowLo, rowHi, outPx);
    };
    GSRowPool::instance().run(0, texH - 1, lanes, joinedDecode);

    static const bool s_g298Verify =
        envFlagEnabled("DC2_G298_VERIFY_TEXDECODE");
    if (s_g298ParTexDecode && s_g298Verify && texW > 0 && texH > 0)
    {
        static std::vector<uint32_t> s_shadow;
        s_shadow.resize(static_cast<size_t>(texW) * texH);
        decodeRows(0, texH - 1, s_shadow.data());
        uint64_t bad = 0u;
        for (size_t i = 0; i < s_shadow.size(); ++i)
            bad += outPx[i] != s_shadow[i] ? 1u : 0u;
        static std::atomic<uint64_t> s_calls{0u}, s_pixels{0u}, s_bad{0u};
        const uint64_t n =
            s_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t totalPixels =
            s_pixels.fetch_add(static_cast<uint64_t>(pixels),
                               std::memory_order_relaxed) +
            static_cast<uint64_t>(pixels);
        const uint64_t totalBad =
            s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
        if (bad != 0u || n <= 8u || (n % 1024u) == 0u)
            std::fprintf(stderr,
                         "[G298:decode-verify] calls=%llu pixels=%llu bad=%llu "
                         "last=%dx%d lanes=%d bad=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(totalPixels),
                         static_cast<unsigned long long>(totalBad),
                         texW, texH, lanes,
                         static_cast<unsigned long long>(bad));
    }
}

// Per-entry validation + draw-state derivation. Returns false → whole flush falls back to CPU.
struct G178EntryState
{
    uint64_t texKey = 0;
    int texW = 0, texH = 0;
    uint8_t blend = 0, tfx = 0, tcc = 0;
    uint8_t depthFunc = 0;
    bool depthWrite = false, bilinear = false;
    uint8_t wrapU = 0, wrapV = 0;
    uint16_t scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
    uint16_t zbpPage = 0;
    uint8_t zpsm = 0;
    bool guestDepth = false;
    bool skip = false; // valid but draws nothing (e.g. ZTST=NEVER)
};