// ===== G632: COHERENCE EPOCHS — cut the NUMBER of CPU-visible publication boundaries ==========
//
// PREMISE (G631 §"Remaining Blocker"). The persistent GPU-authoritative GS domain (G630) is
// +6.08 ms/f against its non-resident control, and the residual is NOT bytes — G631 already cut
// raw-VRAM readback volume 38x (57,108 -> 1,504 MiB/run) and the loss did not follow. It is the
// COUNT of synchronous GPU->CPU publications, each of which is a round trip queued behind the
// shared G178 GL FIFO and therefore prices at ~0.24-1.28 ms independent of its size:
//
//     [G630:break]  cause   mats   pages     ms      ms/mat  pages/mat
//                   tex     5,957  139,665   4,313.7  0.72    23.4
//                   up      2,899   17,781     858.2  0.30     6.1
//                   local   1,303    2,924   1,607.6  1.28     2.2     <- 1.28 ms for 18 KiB
//                   gpuin   1,052    3,603     252.3  0.24     3.4
//                   cpu       570   31,744     256.6  0.45    55.7
//
// ~11,781 breaks/run ~= 7.3 s of front-end blocking ~= 2.2 ms/f, plus the fence inflation it
// forces on the ordinary colour readback.
//
// THE TWO MOVES THIS FILE MAKES
//
//   1. EPOCH BATCHING. Several of those breaks are issued from PREPARE passes that publish for a
//      consumer running LATER in the same flush — `g630PrepareCpuReplay` and
//      `g630PrepareGpuReplayInputs` walk the whole draw list calling the publisher once (three
//      times, in the CPU case) PER ENTRY. Nothing between two such calls reads or writes a guest
//      VRAM byte: the loop body is pure GS geometry (`g144TargetRange`, `g254DepthSurface`,
//      `g144TextureRange`, `g144ClutRange`). So the union of their wanted sets can be published in
//      ONE round trip at the end of the pass.
//
//      That union is the IDENTICAL page set, not a superset: a page's dirty bit is only ever
//      CLEARED by publication and only ever SET by `g630SubmitReplay`, and no replay is submitted
//      inside a prepare pass. A page dirty at entry k was therefore dirty at entry 0 as well.
//      Exact by construction — this can only change HOW MANY round trips carry the same bytes.
//
//   2. THE `tex` CLASS, WHICH IS HALF OF EVERY BREAK. `rasterizer_vram_materialization.inc`'s
//      texture bind pre-pass publishes a draw's texture pages whenever BOTH direct-view gates
//      decline — `g630TryBind` (CT32 linear mirror) and `g630TryT8View` (raw PSMT8). G628's law
//      says a first-fail bucket cannot name the blocker of a conjunctive admission, so before a
//      line of admission code is written both gates are refactored to compute EVERY failing
//      conjunct into one bitmask and to return `mask == 0` as their admission. The shipped
//      predicate IS the censused predicate (G629's law: do not restate a predicate, call it).
//
// FLAGS
//   DC2_G632_STAT=1       behaviour-pure census: reject-mask histograms + epoch potential.
//   DC2_G632_EPOCH=1      arm epoch batching on the two prepare passes. Rollback once promoted:
//                         DC2_G632_NO_EPOCH=1.
//   DC2_G632_TEXEPOCH=1   arm epoch batching on the texture bind pre-pass. Rollback once promoted:
//                         DC2_G632_NO_TEXEPOCH=1. Held separate because that pass is NOT pure —
//                         see the conflict census below.
//   DC2_G632_VERIFY=1     exactness oracle: every epoch flush is compared page-for-page against
//                         the per-call publication it replaced.
//
// ⚠️ WHY THE TEXTURE PRE-PASS IS A SEPARATE ARM. Its loop body is not pure: `g261Materialize`
// (the legacy G261 residency publication) WRITES guest VRAM from an FBO, and `g623TryBind` /
// `g630TryBind` read PAGE GENERATIONS, which a publication bumps. Deferring a G630 publication
// past either one reorders two writers of the same page, or changes a later bind decision. So the
// census counts exactly that: how many `g261Materialize` calls land inside a pre-pass that has a
// non-empty deferred set, and whether their target range OVERLAPS it. `conflict=0` over a route is
// the evidence that the reorder is unobservable; anything else closes this arm.
//
// This file is included from g630_gpu_domain.inc (it needs nothing from it, but everything that
// uses it lives there and in the texture pre-pass).

// ---- gates --------------------------------------------------------------------------------
// Namespace-scope caches (G619's law: a function-local `static const bool` is a per-access MSVC
// `_Init_global_epoch` TLS guard, not a cache). These are read per draw in the texture pre-pass.
static const bool s_g632Stat = envFlagEnabled("DC2_G632_STAT");
static const bool s_g632Verify = envFlagEnabled("DC2_G632_VERIFY");
// PROMOTION STATE: both levers default-OFF until this phase's paired gate runs. Flip the constant
// to promote; the NO_ rollback must keep working afterwards.
static constexpr bool kG632EpochDefaultOn = false;
static constexpr bool kG632TexEpochDefaultOn = false;
static const bool s_g632Epoch = !envFlagEnabled("DC2_G632_NO_EPOCH") &&
                                (kG632EpochDefaultOn || envFlagEnabled("DC2_G632_EPOCH"));
static const bool s_g632TexEpoch = !envFlagEnabled("DC2_G632_NO_TEXEPOCH") &&
                                   (kG632TexEpochDefaultOn || envFlagEnabled("DC2_G632_TEXEPOCH"));

static bool g632StatOn() { return s_g632Stat || s_g632Verify; }
static bool g632VerifyOn() { return s_g632Verify; }
static bool g632EpochOn() { return s_g632Epoch; }
static bool g632TexEpochOn() { return s_g632TexEpoch; }

static constexpr uint32_t kG632Pages = 512u;

// ---- reject-conjunct bit sets ---------------------------------------------------------------
// One bit per conjunct of the shipped admission, in source order. A draw that fails several sets
// several bits — that is the whole point (G628: `[G628:bits]` named THREE blockers where the
// first-fail bucket named one useless composite).
enum : uint32_t
{
    // g630TryBind — the CT32 linear-mirror direct view.
    kG632BindOff       = 1u << 0,  // domain off / faulted
    kG632BindAlready   = 1u << 1,  // another mechanism already bound this entry
    kG632BindNoTme     = 1u << 2,  // untextured draw
    kG632BindSkip      = 1u << 3,  // entry state says skip
    kG632BindPsm       = 1u << 4,  // TEX0.PSM is not CT32
    kG632BindNoFst     = 1u << 5,  // not FST (perspective-corrected UV)
    kG632BindNoSurface = 1u << 6,  // no resident G630 surface with this tbp0/tbw
    kG632BindSelf      = 1u << 7,  // source surface IS the destination
    kG632BindTexDim    = 1u << 8,  // texW/texH not positive
    kG632BindTexWide   = 1u << 9,  // texW wider than the surface
    kG632BindNoCov     = 1u << 10, // surface has no covered rows yet
    kG632BindWindow    = 1u << 11, // sampled window not provably inside the surface
    kG632BindRange     = 1u << 12, // texture block range invalid / empty view rows
    kG632BindView      = 1u << 13, // g630PrepareView refused (impure, set by the caller)

    // g630TryT8View — the raw PSMT8 decode view.
    kG632T8Off         = 1u << 16,
    kG632T8Uninit      = 1u << 17, // raw image not seeded yet
    kG632T8NoTme       = 1u << 18,
    kG632T8Skip        = 1u << 19,
    kG632T8Psm         = 1u << 20, // TEX0.PSM is not T8
    kG632T8NoKey       = 1u << 21, // no texture key
    kG632T8Dim         = 1u << 22, // texW/texH outside 1..1024
    kG632T8Range       = 1u << 23, // texture block range invalid
    kG632T8NoAuthority = 1u << 24, // no page of the texture is GPU-authoritative
    kG632T8Clut        = 1u << 25, // CLUT block range invalid
    kG632T8Build       = 1u << 26, // permutation/backend build refused (impure)

    kG632BindMask      = 0x0000ffffu,
    kG632T8Mask        = 0xffff0000u,
};

// g630CanPatchUpload — the IMAGE-upload overlap gate that suppresses an `up` break by expressing
// the upload ON the raw image instead of publishing under it.
enum : uint32_t
{
    kG632UpOff      = 1u << 0,
    kG632UpUninit   = 1u << 1,
    kG632UpNoDirty  = 1u << 2, // nothing GPU-authoritative anywhere (then there is no break either)
    kG632UpUnknown  = 1u << 3, // destination rect unresolvable
    kG632UpPsm      = 1u << 4, // not CT32
    kG632UpFbw      = 1u << 5,
    kG632UpEmpty    = 1u << 6, // zero width/height
    kG632UpRange    = 1u << 7, // g144RangeForRect refused
    kG632UpClean    = 1u << 8, // rect resolves but no page under it is GPU-authoritative
};

// ---- mask histograms ------------------------------------------------------------------------
// Worker-thread only (the whole G630 domain is), so a plain array is correct here; the overflow
// counter is what makes a truncated histogram readable instead of a lie.
struct G632MaskRow
{
    uint64_t mask = 0u;
    uint64_t n = 0u;
};

static std::array<G632MaskRow, 40> g_g632TexRows{};
static uint32_t g_g632TexRowCount = 0u;
static uint64_t g_g632TexRowOverflow = 0u;
static uint64_t g_g632TexDraws = 0u;    // draws that reached the tex fallback
static uint64_t g_g632TexBreaks = 0u;   // ... and had at least one GPU-authoritative texture page

static std::array<G632MaskRow, 16> g_g632UpRows{};
static uint32_t g_g632UpRowCount = 0u;
static uint64_t g_g632UpRowOverflow = 0u;
static uint64_t g_g632UpCalls = 0u;

static void g632AddRow(G632MaskRow *rows, uint32_t cap, uint32_t &count, uint64_t &overflow,
                       uint64_t mask)
{
    for (uint32_t i = 0u; i < count; ++i)
        if (rows[i].mask == mask) { ++rows[i].n; return; }
    if (count < cap) { rows[count].mask = mask; rows[count].n = 1u; ++count; return; }
    ++overflow;
}

static void g632NoteTexReject(uint32_t bindMask, uint32_t t8Mask, bool hadAuthority, uint32_t psm)
{
    if (!g632StatOn()) return;
    ++g_g632TexDraws;
    if (hadAuthority) ++g_g632TexBreaks;
    // The TEX0.PSM rides in the key so the "neither CT32 nor T8" class names its own format
    // instead of being one anonymous bucket — the same mistake in miniature that G628's first-fail
    // census made at the conjunct level.
    const uint64_t mask = (static_cast<uint64_t>(t8Mask) & 0xffffffffull) |
                          (static_cast<uint64_t>(bindMask) & 0xffffull) |
                          (hadAuthority ? (1ull << 32) : 0ull) |
                          ((static_cast<uint64_t>(psm) & 0x3full) << 40);
    g632AddRow(g_g632TexRows.data(), static_cast<uint32_t>(g_g632TexRows.size()),
               g_g632TexRowCount, g_g632TexRowOverflow, mask);
}

static void g632NoteUploadReject(uint32_t mask)
{
    if (!g632StatOn()) return;
    ++g_g632UpCalls;
    g632AddRow(g_g632UpRows.data(), static_cast<uint32_t>(g_g632UpRows.size()),
               g_g632UpRowCount, g_g632UpRowOverflow, mask);
}

// ---- the epoch accumulator --------------------------------------------------------------------
// One epoch is open at a time; the three passes that use it never nest. `calls` is how many
// publications the pass WOULD have issued (the ones that actually had a dirty wanted page), and
// `flushes` is how many it actually issued — their difference is the deleted round trips.
struct G632EpochState
{
    std::array<uint8_t, kG632Pages> wanted{};
    // DC2_G632_VERIFY=1 oracle state. The batching premise is precisely "no page becomes
    // GPU-authoritative while the epoch is open" — that is what makes the union of the per-call
    // wanted sets the IDENTICAL page set rather than a superset. Snapshot the authority map at
    // begin and re-check it at every add and at the flush.
    std::array<uint8_t, kG632Pages> dirtyAtBegin{};
    bool open = false;
    bool defer = false; // false = census-only: observe the union, publish each call as shipped
    bool any = false;   // at least one wanted page is GPU-authoritative
    int cause = 0;
    int slot = 0;       // 0 = cpu replay, 1 = gpu replay inputs, 2 = texture pre-pass
};

static G632EpochState g_g632Ep;

static std::array<uint64_t, 3> g_g632EpCalls{};    // deferred publications that had dirty pages
static std::array<uint64_t, 3> g_g632EpFlushes{};  // round trips actually issued
static std::array<uint64_t, 3> g_g632EpPasses{};   // epochs opened
static std::array<uint64_t, 3> g_g632EpPages{};    // pages in the flushed union
static uint64_t g_g632TexConflicts = 0u;           // g261Materialize inside an open tex epoch
static uint64_t g_g632TexConflictOverlap = 0u;     // ... and its range overlaps the deferred set
static uint64_t g_g632VerifyChecks = 0u;
static uint64_t g_g632VerifyBad = 0u;

// The resident domain's own CPU cost ON THE POLE THREAD. `[G630:domain] binds=` reaches millions
// per run, and every one of them walks the surface map, solves a sample window and sums a page-
// generation range before it can answer "the FBO is still equal to guest VRAM". A publication
// census alone cannot see this: it is work the resident arm ADDS, not a boundary it crosses.
static uint64_t g_g632GateNs[2]{};    // 0 = g630TryBind, 1 = g630TryT8View
static uint64_t g_g632GateCalls[2]{};

// The two halves of a publication, separately clocked. G631 called `g630Poll(true)` + the
// synchronous read a "double wait"; that is a claim about where the time is, and it has to be
// measured before it can be deleted. `poll` is the blocking drain of every in-flight G630 future;
// `read` is the backend round trip that follows it; `commit` is the memcpy + page-generation bump.
static uint64_t g_g632PubNs[3]{};     // 0 = poll, 1 = read, 2 = commit
static uint64_t g_g632PubCalls[3]{};

struct G632GateTimer
{
    int slot;
    std::chrono::steady_clock::time_point t0;
    explicit G632GateTimer(int s)
        : slot(s), t0(s_g632Stat ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{})
    {
    }
    ~G632GateTimer()
    {
        if (!s_g632Stat) return;
        g_g632GateNs[slot] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
        ++g_g632GateCalls[slot];
    }
};

static const char *g632SlotName(int slot)
{
    switch (slot)
    {
    case 0: return "cpu";
    case 1: return "gpuin";
    default: return "tex";
    }
}

// The texture pre-pass calls this immediately before every legacy G261 publication so the reorder
// hazard is COUNTED rather than assumed away. `range` is the target's block range.
static void g632NoteG261InEpoch(const G144BlockRange &r)
{
    if (!g_g632Ep.open || g_g632Ep.slot != 2 || !g_g632Ep.any) return;
    ++g_g632TexConflicts;
    if (!r.valid) { ++g_g632TexConflictOverlap; return; }
    const uint32_t lo = std::min(kG632Pages - 1u, r.first >> 5u);
    const uint32_t hi = std::min(kG632Pages - 1u, r.last >> 5u);
    for (uint32_t p = lo; p <= hi; ++p)
        if (g_g632Ep.wanted[p] != 0u) { ++g_g632TexConflictOverlap; return; }
}

static void g632Report()
{
    if (!g632StatOn()) return;
    std::fprintf(stderr,
                 "[G632:texbits] draws=%llu breaks=%llu rows=%u overflow=%llu\n",
                 (unsigned long long)g_g632TexDraws, (unsigned long long)g_g632TexBreaks,
                 g_g632TexRowCount, (unsigned long long)g_g632TexRowOverflow);
    for (uint32_t i = 0u; i < g_g632TexRowCount; ++i)
        std::fprintf(stderr, "[G632:texbits]   bind=0x%04x t8=0x%04x auth=%u psm=%02u n=%llu\n",
                     static_cast<unsigned>(g_g632TexRows[i].mask & 0xffffu),
                     static_cast<unsigned>((g_g632TexRows[i].mask >> 16) & 0xffffu),
                     static_cast<unsigned>((g_g632TexRows[i].mask >> 32) & 1u),
                     static_cast<unsigned>((g_g632TexRows[i].mask >> 40) & 0x3fu),
                     (unsigned long long)g_g632TexRows[i].n);
    std::fprintf(stderr, "[G632:up] calls=%llu rows=%u overflow=%llu\n",
                 (unsigned long long)g_g632UpCalls, g_g632UpRowCount,
                 (unsigned long long)g_g632UpRowOverflow);
    for (uint32_t i = 0u; i < g_g632UpRowCount; ++i)
        std::fprintf(stderr, "[G632:up]   mask=0x%04x n=%llu\n",
                     static_cast<unsigned>(g_g632UpRows[i].mask & 0xffffu),
                     (unsigned long long)g_g632UpRows[i].n);
    for (int s = 0; s < 3; ++s)
    {
        if (g_g632EpPasses[s] == 0u && g_g632EpCalls[s] == 0u) continue;
        std::fprintf(stderr,
                     "[G632:epoch] pass=%-5s on=%u epochs=%llu deferred=%llu flushes=%llu "
                     "pages=%llu saved=%lld\n",
                     g632SlotName(s),
                     (s == 2 ? (g632TexEpochOn() ? 1u : 0u) : (g632EpochOn() ? 1u : 0u)),
                     (unsigned long long)g_g632EpPasses[s],
                     (unsigned long long)g_g632EpCalls[s],
                     (unsigned long long)g_g632EpFlushes[s],
                     (unsigned long long)g_g632EpPages[s],
                     (long long)g_g632EpCalls[s] - (long long)g_g632EpFlushes[s]);
    }
    if (g_g632TexConflicts != 0u || g632TexEpochOn())
        std::fprintf(stderr, "[G632:conflict] g261InTexEpoch=%llu overlapping=%llu\n",
                     (unsigned long long)g_g632TexConflicts,
                     (unsigned long long)g_g632TexConflictOverlap);
    std::fprintf(stderr,
                 "[G632:wait] poll=%llu/%.1fms (%.3fms) read=%llu/%.1fms (%.3fms) "
                 "commit=%llu/%.1fms\n",
                 (unsigned long long)g_g632PubCalls[0], (double)g_g632PubNs[0] / 1e6,
                 g_g632PubCalls[0] ? (double)g_g632PubNs[0] / 1e6 / (double)g_g632PubCalls[0] : 0.0,
                 (unsigned long long)g_g632PubCalls[1], (double)g_g632PubNs[1] / 1e6,
                 g_g632PubCalls[1] ? (double)g_g632PubNs[1] / 1e6 / (double)g_g632PubCalls[1] : 0.0,
                 (unsigned long long)g_g632PubCalls[2], (double)g_g632PubNs[2] / 1e6);
    std::fprintf(stderr,
                 "[G632:gate] bind=%llu/%.1fms (%.3fus) t8=%llu/%.1fms (%.3fus)\n",
                 (unsigned long long)g_g632GateCalls[0], (double)g_g632GateNs[0] / 1e6,
                 g_g632GateCalls[0] ? (double)g_g632GateNs[0] / 1e3 / (double)g_g632GateCalls[0]
                                    : 0.0,
                 (unsigned long long)g_g632GateCalls[1], (double)g_g632GateNs[1] / 1e6,
                 g_g632GateCalls[1] ? (double)g_g632GateNs[1] / 1e3 / (double)g_g632GateCalls[1]
                                    : 0.0);
    if (g632VerifyOn())
        std::fprintf(stderr, "[G632:verify] checks=%llu bad=%llu\n",
                     (unsigned long long)g_g632VerifyChecks,
                     (unsigned long long)g_g632VerifyBad);
}
