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
void g178NoteVramWriteRectTagged(uint8_t *vram, uint32_t bpBlocks, uint32_t bw64,
                                 uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                 uint32_t h, uint32_t g570Source);
// G476 paired A/B arm for the run-weighted page walk. Defined with EXTERNAL linkage in
// g419_ab_instrument.inc, which this TU includes after this file. It must be declared in this
// GLOBAL-scope gap: a declaration inside the anonymous namespace below names a different,
// never-defined symbol.
int g476PageBumpArm();
// G595 paired A/B arms, declared in the same GLOBAL-scope gap and for the same reason (the G568
// lesson): `g595line` alternates G270's display-LINE deferral, `g595note` alternates the
// state-identity memo for the per-entry page-range append. Both are defined with EXTERNAL linkage
// in g419_ab_instrument.inc, which this TU includes 91 includes AFTER this file.
int g595LineArm();
int g595NoteMemoArm();
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
// G595 PROMOTION: DEFAULT-ON under the native-renderer graph. `DC2_G270_LINE_WAVE=1` is retained
// as a redundant compatibility opt-in; the rollbacks are DC2_G595_NO_LINEDEFER=1 (this phase) and
// the original DC2_G270_NO_LINE_WAVE=1. Same shape as g286TransientTargetOn's G287 promotion.
bool g270LineWaveOn()
{
    static const bool s_on = g260NrOn() &&
                             !envFlagEnabled("DC2_G595_NO_LINEDEFER") &&
                             !envFlagEnabled("DC2_G270_NO_LINE_WAVE") &&
                             !envFlagDisabled("DC2_G270_LINE_WAVE");
    return s_on;
}

// ⭐⭐⭐ G595: G270 WAS REFUTED ON A ROUTE THAT HAS NO LINES.
//
// The 2026-07-16 verdict ("default-off — no payoff on MAP-0") is a MAP-0 property, and G568's law
// says a direction refuted on one route may lead on another. `[G526:draw]`/`[G526:split]`
// de-cumulated over `s05 n=1021..2461` (tools/g595_draw.py) prices the inline exit at
//
//     4.2 calls/frame x 242.4 us = 1.007 ms/f — 18.3% of the whole 5.50 ms/f draw+capture layer
//
// and `[G526:type]` says all of them are LINESTRIP, `[G526:inl]` that they are exactly two shapes:
// fbp=0x0 and fbp=0x68 (the two display buffers), fpsm=CT32, tme=0, abe=0, 464 px each. That is
// G270's admission predicate verbatim. The cost is NOT the raster (15 us of the 242): it is
// `drain` 51.8 us + `edge` 175.4 us — every one of those four primitives forces a full pending-
// graph drain plus the g278/g289/g272/g277/g276/g261/g242 ownership+materialize edge, four times
// a frame, because a LINE is not deferrable. Measured with the lever on, the whole inline bucket
// disappears and the layer reads 5.498 -> 4.358 ms/f (-1.140, -20.7%) with the deferred count up
// by exactly the missing lines.
//
// `g270LineWaveOn()` stays the cached env/default accessor. `g270LineWaveActive()` is what the
// capture predicate and the whole-flush classifier read, so the paired A/B can alternate it.
// The arm is frame-stable (g419AbTick runs once per frame, hold 16) and every batch drains inside
// the frame that captured it, so an entry can never be captured under one arm and classified
// under the other.
// The arm accessor is hoisted behind a one-shot cached bool so the shipped path does not call
// g595LineArm() per draw just to learn that no A/B is running (G502, applied to a lever's own
// selector). ⚠️ This was built to recover a +0.15 ms/f prologue regression and DID NOT: `pro`
// reads 1.783 hoisted / 1.773 hoisted+reordered against a 1.633 control. Both that hypothesis and
// the conjunct-order one are REFUTED — the residual is unattributed (G595 §6). Kept as the better
// shape, not as a fix.
bool g270LineWaveActive()
{
    static const bool s_underAb = (g595LineArm() >= 0);
    if (!s_underAb)
        return g270LineWaveOn();
    return g260NrOn() && g595LineArm() == 1;
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

// ===== G573: RIDEPOD TRANSIENT 0x13b TRIANGLE ADMISSION =====
//
// G286/G287 proved the immediate-readback transient target, but capture deliberately admitted only
// opaque sprites. Ridepod's boss mesh is the same CT32 target and is almost entirely textured,
// blended TRIANGLE/TRISTRIP draws. Those primitives currently force one graph drain + ownership
// edge each before scalar inline rasterization. This gate changes admission only: the existing
// whole-flush classifier still fails closed to exact banded CPU replay, and a successful GPU flush
// retains G286's immediate readback/page-publication contract.
//
// G573 PROMOTION (2026-08-13): DEFAULT-ON. Rollback is DC2_G573_NO_TRI_13B=1.
// Gated on the Ridepod boss with `tools/g525_route.ps1 -Route ridepod -Ab tri13b`:
//   cand 52.871 ms/f (n=311) vs ctrl 61.568 (n=271) -> -8.697 ms/f, -14.13%, seDelta 0.642 (~13.5 sigma)
//   blocked(w=6 mean=-6.255 se=1.314 t=-4.76 fav=6)   used(... 3=0), i.e. no interval saw both arms.
// The control arm reproduces G572's independently measured 62.23 ms/f default for the same window.
// Divergence proof: [G573:admit] n=2,228,224 (tri 852,881 / strip 1,375,343), all inside n=1321..1921.
//
// WHY THIS IS SAFE. [G290:probe] measures 23,996,467 CPU-replayed entries against those admissions:
// the boss mesh is DEPTH-TESTED, and a batch needing real guest depth "has already failed closed"
// (rasterizer_vram_materialization.inc:4438), so the admitted primitives are consumed by the exact
// banded CPU replay -- the SAME scalar rasterizer, in submission order. Admission changes WHEN and
// IN WHAT GROUPING the work runs, not how a pixel is computed. Ownership is untouched:
// [G261:invariant] escaped writer = 0 on every run (3 runs), 0 crash lines (4 runs).
//
// ⛔ TWO GATES ARE NOT AVAILABLE HERE AND MUST NOT BE CLAIMED AS PASSED:
//  1. The G286 same-run pixel oracle NEVER FIRES on this population -- it is gated `!guestDepth`
//     (that file, :4441 and :5407) and this geometry is depth-tested. Zero checks over 2.2 M
//     admitted primitives. It gates the depth-less SPRITE class only.
//  2. A cross-arm pixel comparison CANNOT gate this route. Measured: on-vs-off median 92.85% of
//     pixels differing -- but the DETERMINISM CONTROL (same binary, same flags, two runs) measures
//     92.10%. The boss fight's AI/RNG does not replay, so equal host ticks are not equal scenes.
//     `ridepod`'s boss window joins route3 as unusable for a pixel A/B.
// Correctness therefore rests on: the exact-CPU-replay argument above, zero ownership invariants,
// and direct normal-output review of BOTH arms (full route, 14 boss frames each: Linda's textures
// and decals, Ridepod chassis, the alpha-blended spotlight cone, floor shadows, depth ordering,
// HUD, damage numbers -- clean in both, neither arm showing a defect the other lacks).
static bool g573Tri13bOn()
{
    static const bool paired = []()
    {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "tri13b") == 0;
    }();
    if (paired)
        return g573Tri13bArm() > 0;
    static const bool on = !envFlagEnabled("DC2_G573_NO_TRI_13B") &&
                           !envFlagDisabled("DC2_G573_TRI_13B");
    return on && g286TransientTargetOn();
}

static bool g573Tri13bStatOn()
{
    static const bool on = envFlagEnabled("DC2_G573_STAT");
    return on;
}

static void g573NoteTri13bAdmission(uint32_t prim)
{
    if (!g573Tri13bStatOn())
        return;
    // GS-worker only. Plain counters keep diagnostics out of paired/default timing.
    static uint64_t total = 0u, tri = 0u, strip = 0u;
    ++total;
    tri += prim == GS_PRIM_TRIANGLE ? 1u : 0u;
    strip += prim == GS_PRIM_TRISTRIP ? 1u : 0u;
    if (total <= 8u || (total % 65536u) == 0u)
        std::fprintf(stderr,
                     "[G573:admit] n=%llu tri=%llu strip=%llu\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(tri),
                     static_cast<unsigned long long>(strip));
}

// ===== G570: transient GPU execution for the route2 0x13d combat target =====
//
// G569 made 0x13d persistent and removed its large CPU replay, but that target physically
// overlaps the resident 0x139 family. A later 0x139 texture materialization then advanced pages
// underneath dirty 0x13d residency, producing the jump/dash escaped-writer defect. G570 keeps
// guest VRAM authoritative instead: materialize aliases before the batch, render into the normal
// backend FBO, read color back in the same flush, and publish page generations immediately.
// Nothing is added to g248TargetIndex and no G261 ownership survives the flush.
//
// Bring-up is opt-in until the exact same-run oracle and route gates pass. The verifier implies
// the path so DC2_G570_VERIFY_13D=1 cannot accidentally compare the CPU path with itself.
bool g570Transient13dOn()
{
    static const bool s_on =
        (envFlagEnabled("DC2_G570_TRANSIENT_13D") || g570Verify13dOn()) &&
        !envFlagEnabled("DC2_G570_NO_TRANSIENT_13D") &&
        !envFlagDisabled("DC2_G570_TRANSIENT_13D");
    return s_on && g260NrOn();
}

bool g570Transient13dTarget(uint32_t fbp)
{
    return g570Transient13dOn() && fbp == 0x13du;
}

// G570: the large combat/jump 0x139 batch was still rejected as a physical color/Z alias even
// though G403/G411 deliberately moved ZBP=0xd0 into a private mirror.  Bring up the narrow
// private-depth admission independently; its same-run verifier implies the path.
bool g570Private139On()
{
    static const bool s_on =
        (envFlagEnabled("DC2_G570_PRIVATE_139") || g570Verify139On()) &&
        !envFlagEnabled("DC2_G570_NO_PRIVATE_139") &&
        !envFlagDisabled("DC2_G570_PRIVATE_139");
    return s_on && g260NrOn();
}

static bool g570Transient13dStatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G570_TRANSIENT_STAT") ||
                             g570Verify13dOn();
    return s_on;
}

// ===== G586: transient GPU execution for the measured 0x141 Ridepod RTT =====
//
// G583's post-promotion whole-population census leaves 0x141 as 78.4% of replayed fallback
// entries.  G569 proved that making this alias-family target persistently resident is unsafe:
// guest writers can escape while GPU ownership survives.  This path therefore reuses G570's
// opposite ownership contract: materialize overlapping residents before the batch, render into
// the ordinary backend FBO, immediately read colour back to guest VRAM, publish generations, and
// retain no residency after the flush.  The exact verifier implies the path; an explicitly armed
// G569 slot disables it so persistent and transient ownership can never be mixed.
bool g586Transient141On()
{
    static const bool s_on =
        (envFlagEnabled("DC2_G586_TRANSIENT_141") || g586Verify141On()) &&
        !envFlagEnabled("DC2_G586_NO_TRANSIENT_141") &&
        !envFlagDisabled("DC2_G586_TRANSIENT_141");
    return s_on && g260NrOn() && g248TargetIndex(0x141u) < 0;
}

bool g586Transient141Target(uint32_t fbp)
{
    return g586Transient141On() && fbp == 0x141u;
}

static bool g586Transient141StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G586_STAT") || g586Verify141On();
    return s_on;
}

static void g586NoteTransient141Admission(uint32_t prim)
{
    if (!g586Transient141StatOn())
        return;
    static uint64_t total = 0u, sprite = 0u, tri = 0u, strip = 0u;
    ++total;
    sprite += prim == GS_PRIM_SPRITE ? 1u : 0u;
    tri += prim == GS_PRIM_TRIANGLE ? 1u : 0u;
    strip += prim == GS_PRIM_TRISTRIP ? 1u : 0u;
    if (total <= 8u || (total % 65536u) == 0u)
        std::fprintf(stderr,
                     "[G586:admit141] n=%llu sprite=%llu tri=%llu strip=%llu\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(sprite),
                     static_cast<unsigned long long>(tri),
                     static_cast<unsigned long long>(strip));
}

// G587: bounded follow-up for the next Ridepod reject family.  Keep it independent from G586 so
// 0x141's failed oracle can never be enabled as a side effect of testing 0x15d.
bool g587Transient15dOn()
{
    static const bool s_on =
        (envFlagEnabled("DC2_G587_TRANSIENT_15D") || g587Verify15dOn()) &&
        !envFlagEnabled("DC2_G587_NO_TRANSIENT_15D") &&
        !envFlagDisabled("DC2_G587_TRANSIENT_15D");
    return s_on && g260NrOn() && g248TargetIndex(0x15du) < 0;
}

bool g587Transient15dTarget(uint32_t fbp)
{
    return g587Transient15dOn() && fbp == 0x15du;
}

static bool g587Transient15dStatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G587_STAT") || g587Verify15dOn();
    return s_on;
}

static void g587NoteTransient15dAdmission(uint32_t prim)
{
    if (!g587Transient15dStatOn())
        return;
    static uint64_t total = 0u, sprite = 0u, tri = 0u, strip = 0u;
    ++total;
    sprite += prim == GS_PRIM_SPRITE ? 1u : 0u;
    tri += prim == GS_PRIM_TRIANGLE ? 1u : 0u;
    strip += prim == GS_PRIM_TRISTRIP ? 1u : 0u;
    if (total <= 8u || (total % 65536u) == 0u)
        std::fprintf(stderr,
                     "[G587:admit15d] n=%llu sprite=%llu tri=%llu strip=%llu\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(sprite),
                     static_cast<unsigned long long>(tri),
                     static_cast<unsigned long long>(strip));
}

// G558 experiment: S05's remaining 0x13b pole is the same transient target, but its terminal
// full-screen sprite is an opaque SOLID fill (TME=0).  G286 only admitted textured sprites even
// though the transient backend already supports the untextured draw state and preserves the same
// immediate readback/publication contract.  Keep the widening independently bisectable until the
// exact G286 oracle and route gates pass.
bool g558Solid13bOn()
{
    // G569 promoted to DEFAULT-ON alongside the G553/G556/G557 residency slots — it shipped as an
    // `envFlagEnabled` opt-in that nothing set, while the docs described it as default-ON. Gated on
    // `s05` with those three: -22.0% route-aligned, and four correctness routes clean (held title
    // 211650, MAP-15 0 defects, s05 + Dungeon 6 scene-aligned pixel comparison unbiased).
    static const bool s_on = !envFlagEnabled("DC2_G558_NO_13B_SOLID") &&
                             !envFlagDisabled("DC2_G558_13B_SOLID");
    return s_on && g286TransientTargetOn();
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
static std::atomic<uint64_t> g_g570TransientAttempts{0}, g_g570TransientSuccess{0},
    g_g570TransientRejectShape{0}, g_g570TransientRowsPublished{0};
static std::atomic<uint64_t> g_g586TransientAttempts{0}, g_g586TransientSuccess{0},
    g_g586TransientRejectShape{0}, g_g586TransientRowsPublished{0};
static std::atomic<uint64_t> g_g587TransientAttempts{0}, g_g587TransientSuccess{0},
    g_g587TransientRejectShape{0}, g_g587TransientRowsPublished{0};

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

        // G595: bumped by EVERY reset. `add()` and `markUnknown()` only ever GROW the set, so a
        // stable `seq` proves that everything appended since the value was read is still in here.
        // That is exactly the invariant the G595 note memo needs, and reset() is the ONE place the
        // open sets are cleared (g260CloseOpenBatch) — so no other site has to be plumbed.
        uint32_t seq = 0;
        void reset()
        {
            n = 0;
            unknown = false;
            ++seq;
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

    // ===== G523: BATCH-MERGE HEADROOM CENSUS (DC2_G523_MERGE=1, read-only, behaviour-pure) =====
    // [G523:fit] measured the upload-edge drain at 8.84 ms/f over 33.4 GPU flushes/frame, of which
    // only 0.0078 ms/flush iterates ENTRIES — ~0.23 ms/flush is paid PER FLUSH regardless of what
    // the flush carries. One GPU flush is exactly one closed G260Batch, and a batch closes only on
    // an fbp switch, so adjacent batches NEVER share a target and no adjacent merge exists. The one
    // remaining way to have fewer flushes is to coalesce a batch with an EARLIER same-(fbp,fbw)
    // batch, which is legal only if every batch strictly between them is independent of it.
    // This bounds that lever before anyone builds it (G502): it counts the batches that could
    // legally merge, without reordering anything. Unknown range sets fail closed.
    static bool g523CensusOn()
    {
        static const bool v = envFlagEnabled("DC2_G523_MERGE");
        return v;
    }
    static bool g523SetsOverlap(const G260RangeSet &a, const G260RangeSet &b)
    {
        if (a.unknown || b.unknown)
            return true; // fail closed, exactly as g260RangeSetsConflict does
        for (int i = 0; i < a.n; ++i)
            if (b.overlaps(a.r[i]))
                return true;
        return false;
    }
    // Independent = neither batch writes what the other reads or writes. Read/read is not a
    // conflict. This is the same relation g260RangeSetsConflict applies to a VRAM event, lifted
    // from (src,dst) ranges to a whole batch's recorded range sets.
    static bool g523BatchesIndependent(const G260Batch &x, const G260Batch &y)
    {
        return !g523SetsOverlap(x.wr, y.wr) && !g523SetsOverlap(x.wr, y.rd) &&
               !g523SetsOverlap(x.rd, y.wr);
    }
    // g523NoteMergeHeadroom is DEFINED further down, beside g260ExecuteAll: it walks g_g260Graph,
    // which is declared after this point in the file.
    static void g523NoteMergeHeadroom();

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

    // G528: the EXECUTING batch's own page ranges, published for the duration of one `fn()` call so
    // the flush prologue can scope its publications to what this batch actually reads and writes
    // instead of publishing whole-state (see g528_flush_publish.inc). Same lifetime discipline and
    // the same single-threaded flush-owner contract as g272SetDisplayBatch beside it. Null on any
    // path that did not come through the graph — g528FlushPrologue fails closed on that.
    const G260RangeSet *g_g528Wr = nullptr;
    const G260RangeSet *g_g528Rd = nullptr;
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
        // G314 reuses the already-proven per-batch decomposition while its upload-edge
        // census is active.  The gate is cached and remains completely inert by default.
        static const bool s_on = envFlagEnabled("DC2_G290_PROBE") ||
                                 envFlagEnabled("DC2_G314_CENSUS");
        return s_on;
    }
    uint64_t g_g290PrepNs = 0, g_g290ResolveNs = 0, g_g290GpuOkNs = 0, g_g290GpuFailNs = 0,
             g_g290PubNs = 0, g_g290ReplayNs = 0, g_g290NoteNs = 0;
    uint64_t g_g290GpuOk = 0, g_g290GpuFail = 0, g_g290CpuEntries = 0, g_g290Drains = 0;

    // G314 premise census: characterize only real host->local dependency edges.  A backend
    // upload-node architecture can remove synchronization/publication, but it cannot delete the
    // dependent draws themselves.  Keep those terms separate so another inclusive subtraction
    // cannot mistake required GPU work for a removable upload cost (the G313 lesson).
    enum G314NextKind : uint32_t
    {
        kG314NextGpuRead = 0,
        kG314NextGpuWrite = 1,
        kG314NextGpuReadWrite = 2,
        kG314NextLocalRead = 3,
        kG314NextLocalWrite = 4,
        kG314NextUploadWrite = 5,
        kG314NextUnresolved = 6,
        kG314NextCount = 7,
    };

    struct G314UploadShape
    {
        bool used = false;
        uint32_t dbp = 0u, dbw = 0u, dpsm = 0u;
        uint32_t dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
        uint64_t edges = 0u, conflictWr = 0u, conflictRd = 0u, conflictUnknown = 0u;
        uint64_t batches = 0u, entries = 0u;
        uint64_t gpuOk = 0u, gpuFail = 0u, cpuEntries = 0u;
        uint64_t skipRb = 0u, submits = 0u;
        uint64_t mirrorEdges = 0u, mirrorTargets = 0u;
        uint64_t transportEligible = 0u, residentEligible = 0u;
        uint64_t publicationEvents = 0u;
        uint64_t drainNs = 0u, gpuNs = 0u, replayNs = 0u, submitNs = 0u;
        uint64_t eligibleDrainNs = 0u, eligibleSubmitNs = 0u;
        uint64_t next[kG314NextCount]{};
    };

    static std::array<G314UploadShape, 24> g_g314Shapes{};
    static uint64_t g_g314Edges = 0u;
    static bool g_g314EdgeActive = false;

    struct G314PendingUpload
    {
        bool waitingForComplete = false;
        bool armed = false;
        G314UploadShape *shape = nullptr;
        G144BlockRange range{};
        uint32_t dbp = 0u, dbw = 0u, dpsm = 0u;
        uint32_t dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    };
    static G314PendingUpload g_g314Pending{};

    static bool g314CensusOn()
    {
        static const bool s_on = envFlagEnabled("DC2_G314_CENSUS");
        return s_on;
    }

    static bool g314EdgeActive()
    {
        return g314CensusOn() && g_g314EdgeActive;
    }

    static G314UploadShape *g314FindShape(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                         uint32_t dsax, uint32_t dsay,
                                         uint32_t rrw, uint32_t rrh)
    {
        G314UploadShape *freeShape = nullptr;
        for (G314UploadShape &s : g_g314Shapes)
        {
            if (!s.used)
            {
                if (freeShape == nullptr)
                    freeShape = &s;
                continue;
            }
            if (s.dbp == dbp && s.dbw == dbw && s.dpsm == dpsm &&
                s.dsax == dsax && s.dsay == dsay && s.rrw == rrw && s.rrh == rrh)
                return &s;
        }
        if (freeShape == nullptr)
            freeShape = &g_g314Shapes.back(); // bounded overflow bucket; never changes behavior
        if (!freeShape->used)
        {
            freeShape->used = true;
            freeShape->dbp = dbp;
            freeShape->dbw = dbw;
            freeShape->dpsm = dpsm;
            freeShape->dsax = dsax;
            freeShape->dsay = dsay;
            freeShape->rrw = rrw;
            freeShape->rrh = rrh;
        }
        return freeShape;
    }

    static void g314ResolveNext(G314NextKind kind)
    {
        if (!g314CensusOn() || !g_g314Pending.armed || g_g314Pending.shape == nullptr)
            return;
        ++g_g314Pending.shape->next[static_cast<uint32_t>(kind)];
        g_g314Pending.armed = false;
        g_g314Pending.shape = nullptr;
    }

    static void g314ObservePendingDraw(bool writeHit, bool readHit, bool unknown)
    {
        if (!g314CensusOn() || !g_g314Pending.armed)
            return;
        if (unknown)
        {
            g314ResolveNext(kG314NextUnresolved);
            return;
        }
        if (readHit && writeHit)
            g314ResolveNext(kG314NextGpuReadWrite);
        else if (readHit)
            g314ResolveNext(kG314NextGpuRead);
        else if (writeHit)
            g314ResolveNext(kG314NextGpuWrite);
    }

    static void g314ObserveTransfer(const G144BlockRange &src,
                                    const G144BlockRange &dst,
                                    bool srcUnknown, bool dstUnknown)
    {
        if (!g314CensusOn() || !g_g314Pending.armed)
            return;
        if (srcUnknown || dstUnknown)
        {
            g314ResolveNext(kG314NextUnresolved);
            return;
        }
        if (src.valid && g144RangeOverlaps(src, g_g314Pending.range))
            g314ResolveNext(kG314NextLocalRead);
        else if (dst.valid && g144RangeOverlaps(dst, g_g314Pending.range))
            g314ResolveNext(kG314NextLocalWrite);
    }

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
        bool g314WriteHit = false;
        bool g314ReadHit = false;
        bool g314Unknown = false;
        // color target (written and, under blending/fbmsk, read back) — scissor-bounded
        const G144BlockRange color = g144TargetRange(e);
        if (!color.valid &&
            e.ctx.scissor.x1 >= e.ctx.scissor.x0 && e.ctx.scissor.y1 >= e.ctx.scissor.y0)
        {
            g_g260OpenWr.markUnknown();
            g314Unknown = true;
        }
        else
        {
            g_g260OpenWr.add(color);
            g314WriteHit = g314WriteHit ||
                (g_g314Pending.armed && color.valid &&
                 g144RangeOverlaps(color, g_g314Pending.range));
        }
        // active Z surface (read for ZTST, written unless ZMSK) — treat as write-class
        const G254Surface depth = g254DepthSurface(e);
        if (depth.active)
        {
            if (!depth.range.valid)
            {
                g_g260OpenWr.markUnknown();
                g314Unknown = true;
            }
            else
            {
                g_g260OpenWr.add(depth.range);
                g314WriteHit = g314WriteHit ||
                    (g_g314Pending.armed &&
                     g144RangeOverlaps(depth.range, g_g314Pending.range));
            }
        }
        // texture + CLUT reads
        if (e.prim.tme)
        {
            const G144BlockRange tex = g144TextureRange(e);
            if (!tex.valid)
            {
                g_g260OpenRd.markUnknown();
                g314Unknown = true;
            }
            else
            {
                g_g260OpenRd.add(tex);
                g314ReadHit = g314ReadHit ||
                    (g_g314Pending.armed &&
                     g144RangeOverlaps(tex, g_g314Pending.range));
            }
            if (g144IsPalettedPsm(e.ctx.tex0.psm))
            {
                const G144BlockRange clut = g144ClutRange(e);
                if (!clut.valid)
                {
                    g_g260OpenRd.markUnknown();
                    g314Unknown = true;
                }
                else
                {
                    g_g260OpenRd.add(clut);
                    g314ReadHit = g314ReadHit ||
                        (g_g314Pending.armed &&
                         g144RangeOverlaps(clut, g_g314Pending.range));
                }
            }
        }
        g314ObservePendingDraw(g314WriteHit, g314ReadHit, g314Unknown);
    }

    // =========================================================================================
    // ⭐⭐⭐ G595 — STATE-IDENTITY MEMO FOR THE PER-ENTRY PAGE-RANGE APPEND
    //
    // PREMISE, MEASURED (not argued). `[G526:split]` de-cumulated over `s05 n=1021..2461` with
    // tools/g595_draw.py puts the `note` lap at **71.2 ns x 19,225 captured entries/frame =
    // 1.369 ms/f** — 24.9% of the whole 5.50 ms/f drawPrimitive+capture layer and its second
    // largest leaf. `g336NoteConsumer` early-outs on `!g_g336Armed` (default off), so essentially
    // all of it is g260NoteAppend. `[G510:cls]` over the same window reads **92.80% of 18,638
    // entries/frame carrying byte-identical GS state to their predecessor**.
    //
    // ROOT. EVERY input g260NoteAppend reads lives inside that state group, and NOTHING it reads
    // is per-primitive:
    //   g144TargetRange   <- ctx.frame{fbp,fbw,psm} + ctx.scissor
    //   g254DepthSurface  <- ctx.zbuf + ctx.test + ctx.frame.fbw + ctx.scissor
    //   g144TextureRange  <- ctx.tex0
    //   g144ClutRange     <- ctx.tex0 + texclut
    //   the two gates     <- prim.tme, ctx.tex0.psm
    // It never touches e.v[], e.minY/maxY or e.decoded. So when the state repeats, all four block
    // ranges repeat EXACTLY, bit for bit.
    //
    // EXACTNESS — the skipped call provably changes nothing:
    //  1. `G260RangeSet::add(b)` is IDEMPOTENT for an already-added `b`. Whichever of its three
    //     paths took `b` the first time, `b` is afterwards contained in some interval `r[i]`, so
    //     the merge test `b.first <= r[i].last+1 && r[i].first <= b.last+1` fires and the
    //     min/max assignment is a no-op. It cannot fall through to `r[n++]` or to the capacity
    //     path, so `n` cannot move either.
    //  2. `markUnknown()` is idempotent by inspection.
    //  3. `add()`/`markUnknown()` only ever GROW the set; `reset()` is the only shrink, and it is
    //     called from exactly one place (g260CloseOpenBatch). So `seq` unchanged since the memo
    //     was filled ⇒ the memoized ranges are still inside the live open sets. This is what
    //     makes the memo safe against every `g_g144List.clear()` in the TU without plumbing a
    //     single one of them: the memo keys on the ACCUMULATOR, not on the list.
    //  4. The only other observable is `g314ObservePendingDraw`, which CONSUMES `g_g314Pending`.
    //     It is a pure no-op unless `DC2_G314_CENSUS=1`, so the memo is disarmed whole when that
    //     census is armed (fail closed) rather than trying to replay its side effect.
    //
    // The shadow is a raw byte copy of the entry's state prefix, refreshed by `memcpy` on every
    // miss. That makes the comparison exact ACROSS PADDING TOO: the shadow's padding bytes are
    // copied verbatim out of the same stack-local `G144Entry e` the next entry is built in, and
    // nothing ever writes those bytes, so they agree. A disagreement (e.g. right after a vector
    // reallocation move-constructs entries member-wise) can only cause a MISS, never a false hit.
    //
    // Arm `DC2_G419_AB=g595note`; standalone `DC2_G595_NOTEMEMO=1`; rollback once promoted
    // `DC2_G595_NO_NOTEMEMO=1`; oracle `DC2_G595_NOTEVFY=1`; sub-timer `DC2_G595_CENSUS=1`.
    // =========================================================================================
    static constexpr size_t kG595StateBytes = offsetof(G144Entry, v);
    static constexpr size_t kG595StateQwords = kG595StateBytes / 8u;
    static constexpr size_t kG595StateTail = kG595StateBytes % 8u;
    static_assert(kG595StateTail == 0u || kG595StateTail == 4u,
                  "G595: unexpected G144Entry state-prefix size");

    static bool g595NoteMemoDefaultOn()
    {
        // G595 PROMOTION: DEFAULT-ON. Rollback DC2_G595_NO_NOTEMEMO=1 (or DC2_G595_NOTEMEMO=0).
        // Exactness: [G595:note] vfyChecks=58,167,644 vfyBad=0 on `s05`, each check running the
        // REAL g260NoteAppend against a snapshot and asserting both accumulators are unchanged.
        static const bool on = !envFlagEnabled("DC2_G595_NO_NOTEMEMO") &&
                               !envFlagDisabled("DC2_G595_NOTEMEMO");
        return on;
    }
    static bool g595NoteMemoEligible()
    {
        // §4 above: the memo elides g314ObservePendingDraw, whose side effect is real only under
        // the G314 census. Cached once — never a getenv on the per-entry path.
        static const bool ok = !g314CensusOn();
        return ok;
    }
    static bool g595NoteVerifyOn()
    {
        static const bool on = envFlagEnabled("DC2_G595_NOTEVFY");
        return on;
    }
    static bool g595NoteCensusOn()
    {
        static const bool on = envFlagEnabled("DC2_G595_CENSUS");
        return on;
    }

    alignas(16) static thread_local unsigned char t_g595Prev[kG595StateBytes];
    static thread_local bool t_g595Have = false;
    static thread_local uint32_t t_g595WrSeq = 0u;
    static thread_local uint32_t t_g595RdSeq = 0u;
    // Non-atomic and thread_local: the pole is ONE thread and an atomic RMW per captured entry
    // would be a self-inflicted cost on the very path this memo exists to shorten (G526's rule).
    static thread_local uint64_t t_g595NoteNs = 0u;
    static thread_local uint64_t t_g595NoteEntries = 0u;
    static thread_local uint64_t t_g595NoteHits = 0u;
    static thread_local uint64_t t_g595VfyChecks = 0u;
    static thread_local uint64_t t_g595VfyBad = 0u;

    static bool g595StateEq(const G144Entry &e)
    {
        const uint64_t *a = reinterpret_cast<const uint64_t *>(&e);
        const uint64_t *b = reinterpret_cast<const uint64_t *>(t_g595Prev);
        uint64_t d = 0u;
        for (size_t i = 0; i < kG595StateQwords; ++i)
            d |= a[i] ^ b[i];
        if (kG595StateTail == 4u)
        {
            uint32_t x = 0u, y = 0u;
            std::memcpy(&x, reinterpret_cast<const unsigned char *>(&e) + kG595StateBytes - 4u, 4u);
            std::memcpy(&y, t_g595Prev + kG595StateBytes - 4u, 4u);
            d |= static_cast<uint64_t>(x ^ y);
        }
        return d == 0u;
    }

    // Oracle. Asserts the INVARIANT the skip relies on (G528's law: not "the old path found no
    // work", but "running it would have changed nothing"), by running the REAL function against
    // a snapshot and restoring. noinline so the comparison never lands inside the measured basic
    // block (G519 §5b).
    __declspec(noinline) static void g595VerifyNoteSkip(const G144Entry &e)
    {
        const G260RangeSet wr0 = g_g260OpenWr;
        const G260RangeSet rd0 = g_g260OpenRd;
        g260NoteAppend(e);
        const bool bad = std::memcmp(&g_g260OpenWr, &wr0, sizeof(G260RangeSet)) != 0 ||
                         std::memcmp(&g_g260OpenRd, &rd0, sizeof(G260RangeSet)) != 0;
        g_g260OpenWr = wr0;
        g_g260OpenRd = rd0;
        ++t_g595VfyChecks;
        if (bad)
        {
            ++t_g595VfyBad;
            if (t_g595VfyBad <= 8u)
                std::fprintf(stderr,
                             "[G595:vfy-BAD] n=%llu fbp=0x%x fpsm=0x%x sc=%u,%u..%u,%u tme=%u "
                             "tbp=0x%x wrN=%d rdN=%d\n",
                             (unsigned long long)t_g595VfyBad, e.ctx.frame.fbp,
                             (unsigned)e.ctx.frame.psm, (unsigned)e.ctx.scissor.x0,
                             (unsigned)e.ctx.scissor.y0, (unsigned)e.ctx.scissor.x1,
                             (unsigned)e.ctx.scissor.y1, (unsigned)e.prim.tme, e.ctx.tex0.tbp0,
                             g_g260OpenWr.n, g_g260OpenRd.n);
        }
    }

    static void g595NoteReport()
    {
        std::fprintf(stderr,
                     "[G595:note] entries=%llu hits=%llu (%.2f%%) noteNs=%llu avgNs=%.1f | "
                     "vfyChecks=%llu vfyBad=%llu stateBytes=%zu\n",
                     (unsigned long long)t_g595NoteEntries, (unsigned long long)t_g595NoteHits,
                     t_g595NoteEntries
                         ? 100.0 * (double)t_g595NoteHits / (double)t_g595NoteEntries
                         : 0.0,
                     (unsigned long long)t_g595NoteNs,
                     t_g595NoteEntries ? (double)t_g595NoteNs / (double)t_g595NoteEntries : 0.0,
                     (unsigned long long)t_g595VfyChecks, (unsigned long long)t_g595VfyBad,
                     kG595StateBytes);
        std::fflush(stderr);
    }

    // The single call the capture site makes. With the memo disarmed this is exactly
    // `g260NoteAppend(e)` plus one cached-bool test.
    inline void g595NoteAppend(const G144Entry &e)
    {
        static const bool s_verify = g595NoteVerifyOn();
        // G589's law: a `bad=` is unreadable without its `checks=`. The oracle therefore IMPLIES
        // the reporting path, so a verify run can never print a clean-looking verdict silently.
        static const bool s_census = g595NoteCensusOn() || s_verify;
        static const bool s_eligible = g595NoteMemoEligible();
        const auto t0 = s_census ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        // Same hoist as g270LineWaveActive: the arm accessor must not be called per entry on the
        // shipped path just to answer "no A/B is running".
        static const bool s_underAb = (g595NoteMemoArm() >= 0);
        static const bool s_default = g595NoteMemoDefaultOn();
        const bool memoOn = s_eligible && (s_underAb ? (g595NoteMemoArm() == 1) : s_default);
        if (memoOn && t_g595Have && t_g595WrSeq == g_g260OpenWr.seq &&
            t_g595RdSeq == g_g260OpenRd.seq && g595StateEq(e))
        {
            if (s_census)
                ++t_g595NoteHits;
            if (s_verify)
                g595VerifyNoteSkip(e);
        }
        else
        {
            g260NoteAppend(e);
            if (memoOn)
            {
                t_g595Have = true;
                t_g595WrSeq = g_g260OpenWr.seq;
                t_g595RdSeq = g_g260OpenRd.seq;
                std::memcpy(t_g595Prev, &e, kG595StateBytes);
            }
        }
        if (s_census)
        {
            t_g595NoteNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            if ((++t_g595NoteEntries % 400000ull) == 0ull)
                g595NoteReport();
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

    // G523 batch-merge headroom (declaration + rationale are up beside g523BatchesIndependent).
    // Read-only walk of the closed graph: nothing is reordered, nothing is consumed.
    static void g523NoteMergeHeadroom()
    {
        if (!g523CensusOn())
            return;
        static uint64_t s_drains = 0, s_batches = 0, s_merge = 0, s_blocked = 0,
                        s_nopair = 0, s_entries = 0, s_mergeEntries = 0;
        ++s_drains;
        const size_t nb = g_g260Graph.size();
        for (size_t i = 0; i < nb; ++i)
        {
            const G260Batch &bi = g_g260Graph[i];
            if (bi.entries.empty())
                continue;
            ++s_batches;
            s_entries += bi.entries.size();
            const uint32_t fbwI = bi.entries.front().ctx.frame.fbw;
            size_t j = nb; // latest earlier batch on the same target
            for (size_t k = i; k-- > 0;)
            {
                const G260Batch &bk = g_g260Graph[k];
                if (bk.entries.empty())
                    continue;
                if (bk.fbp == bi.fbp && bk.entries.front().ctx.frame.fbw == fbwI)
                {
                    j = k;
                    break;
                }
            }
            if (j == nb)
            {
                ++s_nopair; // this target appears once in the drained graph — nothing to merge with
                continue;
            }
            bool ok = true;
            for (size_t k = j + 1; k < i && ok; ++k)
                if (!g_g260Graph[k].entries.empty() &&
                    !g523BatchesIndependent(g_g260Graph[k], bi))
                    ok = false;
            if (ok)
            {
                ++s_merge;
                s_mergeEntries += bi.entries.size();
            }
            else
            {
                ++s_blocked;
            }
        }
        if ((s_drains % 2000u) == 0u)
        {
            const double nb2 = static_cast<double>(std::max<uint64_t>(1u, s_batches));
            std::fprintf(stderr,
                         "[G523:merge] drains=%llu batches=%llu batches/drain=%.2f "
                         "entries/batch=%.1f mergeable=%llu (%.1f%%) blocked=%llu (%.1f%%) "
                         "nopair=%llu (%.1f%%) mergeEntries/batch=%.1f\n",
                         static_cast<unsigned long long>(s_drains),
                         static_cast<unsigned long long>(s_batches),
                         nb2 / static_cast<double>(s_drains),
                         static_cast<double>(s_entries) / nb2,
                         static_cast<unsigned long long>(s_merge), 100.0 * s_merge / nb2,
                         static_cast<unsigned long long>(s_blocked), 100.0 * s_blocked / nb2,
                         static_cast<unsigned long long>(s_nopair), 100.0 * s_nopair / nb2,
                         s_merge ? static_cast<double>(s_mergeEntries) / s_merge : 0.0);
        }
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
        // G589: this loop is one DRAIN TRANSACTION. Nothing outside the batches themselves can
        // mutate guest VRAM, residency or ownership while it runs, which is what lets
        // flush-invariant preparation be paid once instead of once per batch.
        g589BeginDrain();
        for (size_t bi = 0; bi < prefixCount; ++bi)
        {
            G260Batch &b = g_g260Graph[bi];
            if (b.entries.empty())
                continue;
            g272PrepareBatch(b);
            g401NoteCompositeEntries(g_g144LastVram, b.entries);
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            g_g528Wr = &b.wr; // G528: scope the flush prologue to this batch (see the decls above)
            g_g528Rd = &b.rd;
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
                    // G527: publication half of the target registry — same predicate as the
                    // admission site, in the same edit (see g527_target_registry.inc).
                    if (!g527DeferTarget(c.frame.fbp))
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRectTagged(vram260,
                                              framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1),
                                              10u); // G582 source 10: g260 publish A, color
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRectTagged(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1),
                                                  11u); // G582 source 11: g260 publish A, depth
                    }
                }
            }
            if (fn)
                fn(); // drains + clears g_g144List
            else
                g_g144List.clear(); // cannot happen (entries imply assigned closures); fail safe
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g528Wr = nullptr; // G528: the batch is no longer executing — fail closed again
            g_g528Rd = nullptr;
            g_g144List.swap(b.entries); // reclaim the (now empty) storage's capacity
            g_g260Pool.push_back(std::move(b.entries));
        }
        g589EndDrain();
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
        g523NoteMergeHeadroom(); // G523: read-only, before any batch is consumed
        static const bool s_seqEnv = (std::getenv("DC2_G144_SEQ") != nullptr);
        const std::function<void()> &fn =
            (allowParallel && !s_seqEnv && g_g144FlushFn) ? g_g144FlushFn : g_g144FlushSeqFn;
        // Keep the pre-G271 whole-graph loop on every established/default path. Prefix execution
        // uses its separate opt-in helper only; this avoids changing vector lifetime/capacity or
        // open-batch state for the promoted G260-G266 stack.
        const bool g290Probe = g290ProbeOn();
        // G589: one DRAIN TRANSACTION (see g589_drain_transaction.inc and the prefix executor).
        g589BeginDrain();
        for (auto &b : g_g260Graph)
        {
            if (b.entries.empty())
                continue;
            const auto g290Tp0 = g290Probe ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            g272PrepareBatch(b);
            g401NoteCompositeEntries(g_g144LastVram, b.entries);
            if (g290Probe)
                g_g290PrepNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g290Tp0)
                        .count());
            g_g144List.swap(b.entries);
            g_g144BlockFbp = b.fbp;
            g272SetDisplayBatch(b.fbp, g_g144List.front().ctx.frame.fbw);
            g_g528Wr = &b.wr; // G528: scope the flush prologue to this batch (see the decls above)
            g_g528Rd = &b.rd;
            ++g_g260BatchesExecuted;
            g_g260EntriesExecuted += g_g144List.size();
            if (g178GpuOn() && !g261WaveOn())
            {
                uint8_t *vram260 = g_g144LastVram;
                for (size_t ei = 0; vram260 && ei < g_g144List.size(); ++ei)
                {
                    const G144Entry &e = g_g144List[ei];
                    const GSContext &c = e.ctx;
                    // G527: publication half of the target registry (second site) — same
                    // predicate as the admission site, in the same edit.
                    if (!g527DeferTarget(c.frame.fbp))
                        continue;
                    if (c.scissor.x1 >= c.scissor.x0 && c.scissor.y1 >= c.scissor.y0)
                    {
                        g178NoteVramWriteRectTagged(vram260, framePageBaseToBlock(c.frame.fbp),
                                              c.frame.fbw, c.frame.psm,
                                              c.scissor.x0, c.scissor.y0,
                                              static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                              static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1),
                                              12u); // G582 source 12: g260 publish B, color
                        if (((c.zbuf >> 32u) & 1u) == 0u)
                            g178NoteVramWriteRectTagged(vram260,
                                                  static_cast<uint32_t>(c.zbuf & 0x1FFu) << 5u,
                                                  c.frame.fbw,
                                                  static_cast<uint32_t>((c.zbuf >> 24u) & 0xFu),
                                                  c.scissor.x0, c.scissor.y0,
                                                  static_cast<uint32_t>(c.scissor.x1 - c.scissor.x0 + 1),
                                                  static_cast<uint32_t>(c.scissor.y1 - c.scissor.y0 + 1),
                                                  13u); // G582 source 13: g260 publish B, depth
                    }
                }
            }
            if (fn)
                fn();
            else
                g_g144List.clear();
            g272SetDisplayBatch(0xFFFFFFFFu, 0u);
            g_g528Wr = nullptr; // G528: the batch is no longer executing — fail closed again
            g_g528Rd = nullptr;
            g_g144List.swap(b.entries);
            g_g260Pool.push_back(std::move(b.entries));
        }
        g589EndDrain();
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

// G471 — the SHIFT form of g178PageDims. Every GS page dimension above is a power of two
// (64/128 wide, 32/64/128 tall), so every `/pw` and `/ph` in the page-range walks is exactly a
// right shift. The compiler cannot make that substitution itself: `pw`/`ph` arrive as runtime
// out-params of a three-way switch, so it must emit a real `div`. G471's PC sampler measured the
// page-range bookkeeping family (g178NoteVramWriteRect / the g144 flush helpers / g149BumpVramGen /
// g242PrepareVramReadRect) at roughly 14% of the GS worker's whole span, and the hot entry
// g178BumpRectImpl runs once or twice per CAPTURED DRAW (~10k/f on MAP-0) with FIVE unsigned
// divisions in its prologue.
//
// Exact by construction: for unsigned n and a power-of-two d, n / d == n >> log2(d). Same page
// indices, same walk, same generation increments — this changes only the instructions used to
// derive them. Verified against the division form by DC2_G471_PAGESHIFT_VERIFY=1.
void g178PageShifts(uint32_t psm, uint32_t &pwSh, uint32_t &phSh)
{
    switch (psm)
    {
    case GS_PSM_T8: pwSh = 7u; phSh = 6u; break;   // 128 x 64
    case GS_PSM_T4: pwSh = 7u; phSh = 7u; break;   // 128 x 128
    case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
        pwSh = 6u; phSh = 6u; break;               // 64 x 64
    default: pwSh = 6u; phSh = 5u; break;          // 64 x 32 (CT32/CT24/Z32/Z24)
    }
}

// Shared prologue of every conservative page-range walk, in shift form. Returns the page
// rectangle [px0..px1] x [py0..py1] and the row stride in pages.
struct G471PageRect
{
    uint32_t widthPages, px0, px1, py0, py1, basePage;
};

static inline G471PageRect g471PageRect(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t pwSh, phSh;
    g178PageShifts(psm, pwSh, phSh);
    G471PageRect r;
    r.widthPages = std::max(1u, (std::max(1u, bw64) << 6u) >> pwSh);
    r.py0 = y >> phSh;
    r.py1 = (y + std::max(1u, h) - 1u) >> phSh;
    r.px0 = x >> pwSh;
    r.px1 = (x + std::max(1u, w) - 1u) >> pwSh;
    r.basePage = bpBlocks >> 5;
    return r;
}

// Bring-up oracle (default OFF): recompute the same page rectangle with the ORIGINAL division
// form and report any disagreement. Kept so the substitution can be re-proved after a PSM or
// page-geometry change rather than re-argued. g471PageShiftVerifyOn() is defined in
// rasterizer_draw_prim_probe.inc, which this file is included after.
static inline void g471VerifyPageRect(const G471PageRect &r, uint32_t bpBlocks, uint32_t bw64,
                                      uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g471PageShiftVerifyOn())
        return;
    uint32_t pw, ph;
    g178PageDims(psm, pw, ph);
    const uint32_t widthPages = std::max(1u, (std::max(1u, bw64) * 64u) / pw);
    const uint32_t py0 = y / ph, py1 = (y + std::max(1u, h) - 1u) / ph;
    const uint32_t px0 = x / pw, px1 = (x + std::max(1u, w) - 1u) / pw;
    if (r.widthPages != widthPages || r.py0 != py0 || r.py1 != py1 ||
        r.px0 != px0 || r.px1 != px1)
    {
        static std::atomic<uint32_t> s_bad{0};
        if (s_bad.fetch_add(1u, std::memory_order_relaxed) < 32u)
            std::fprintf(stderr,
                         "[G471:pageshift-BAD] psm=0x%x bw=%u rect=(%u,%u %ux%u) "
                         "shift=(wp=%u px=%u..%u py=%u..%u) div=(wp=%u px=%u..%u py=%u..%u)\n",
                         psm, bw64, x, y, w, h, r.widthPages, r.px0, r.px1, r.py0, r.py1,
                         widthPages, px0, px1, py0, py1);
    }
}

// ===== G476: the +1 SLACK PAGE MAKES EVERY INTERIOR PAGE PAY TWICE =====
//
// Both page-range walks visit `px` from px0..px1 and touch `pg` AND `pg + 1` — the slack page that
// covers a non-page-aligned block base. Across one row that is 2*(px1-px0+1) accesses over only
// (px1-px0+2) DISTINCT pages, because page `s+j` is reached once as `pg` (at px=j) and once as
// `pg+1` (at px=j-1). Every interior page is therefore visited exactly twice and the two ends
// exactly once.
//
// The bump walk pays that in `lock xadd` — the single most repeated instruction on the GS worker's
// bookkeeping path, since g178BumpRectImpl runs once or twice per CAPTURED DRAW (~10k/f on MAP-0)
// and a full-screen CT32 rect covers ~140 pages, i.e. ~280 atomic RMWs per call.
//
// EXACT BY CONSTRUCTION. Adding 2 once is the same value as adding 1 twice: the generations are
// plain monotone counters and no consumer observes an intermediate state (every reader compares a
// whole SUM taken outside the walk). The masked index of each contribution is computed exactly as
// before, so wrap-around aliasing lands identically. Same final counters, same sums, same
// invalidation verdicts — only the instruction count changes. The row is walked from the ends
// inward so the weights stay obvious: `+1, 2, 2, ..., 2, +1`.
//
// VERDICT (G476): exact and verified, but PRICED OUT — kept DEFAULT-OFF (DC2_G476_PAGERUN=1).
// The visit census below measured 1.2 M walks producing 10.17 M old visits against 7.64 M new ones,
// i.e. only ~940 atomic RMWs removed per frame (~6 us/f at ~20 cycles each). The ratio is 1.33x and
// not 2x because most bump rects are ONE page wide (px0 == px1), where the old loop already does
// the minimum. This site is therefore not the ~4.3% the G446 PC sampler attributed to
// `g178NoteVramWriteRect` — that row is another symbol FOLD (G471's law: a PDB public symbol name
// is a hypothesis). Do not re-open this walk; re-open the attribution instead.
//
// Oracle: DC2_G476_PAGERUN=1 + DC2_G476_VERIFY=1 re-runs the original per-px form into shadow
// counters, compares them, and prints the visit ceiling (the oracle lives inside the candidate
// branch, so it needs the lever armed). Paired gate: DC2_G419_AB=pagebump.
// Both walks are hot, so the default path must not pay a cross-TU call per invocation: whether the
// paired lever is SELECTED is fixed for the process (DC2_G419_AB is read once), so that question is
// answered by one static, and only the selected arm re-reads the alternating value.
static bool g476PageRunOn()
{
    static const bool armed = (g476PageBumpArm() >= 0);
    if (armed)
        return g476PageBumpArm() == 1;
    static const bool on = envFlagEnabled("DC2_G476_PAGERUN");
    return on;
}

static bool g476VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G476_VERIFY");
    return on;
}

// G572 (default-ON, rollback DC2_G572_NO_EXACT_ALIGNED_GEN=1): FRAME bases are page units and many transfer bases also happen to
// be exactly page aligned.  For those bases g471PageRect already names the complete physical-page
// set, so the historical +1 neighbour is pure over-coverage.  Keep the neighbour for genuinely
// block-offset bases until the same exact mapping can be proven for them.
static inline bool g572ExactAlignedGenOn()
{
    static const bool on = !envFlagEnabled("DC2_G572_NO_EXACT_ALIGNED_GEN") &&
                           !envFlagDisabled("DC2_G572_EXACT_ALIGNED_GEN");
    return on;
}

// The weight page `s + j` receives from one row's walk over px0..px1: 1 at each end, 2 inside.
// (px1 >= px0 always, so a single-page row yields weights 1 and 1 over pages s+px0 and s+px0+1 —
// exactly what the old loop's single iteration did.)
static inline uint32_t g476RowWeight(uint32_t j, uint32_t px0, uint32_t px1)
{
    return (j == px0 || j == px1 + 1u) ? 1u : 2u;
}

// Sum of write-generations over a conservative page range for a rect at BLOCK base `bp`.
// Any VRAM write into the range changes the sum (each bump is a monotonic increment).
uint64_t g178GenSumRect(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // G471: shift form of the identical page-rect prologue (see g471PageRect).
    const G471PageRect pr = g471PageRect(bpBlocks, bw64, psm, x, y, w, h);
    g471VerifyPageRect(pr, bpBlocks, bw64, psm, x, y, w, h);
    const uint32_t widthPages = pr.widthPages;
    const uint32_t py0 = pr.py0, py1 = pr.py1, px0 = pr.px0, px1 = pr.px1;
    const uint32_t basePage = pr.basePage;
    uint64_t sum = 0;
    const bool exactAligned = g572ExactAlignedGenOn() && (bpBlocks & 31u) == 0u;
    if (exactAligned)
    {
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t pg = basePage + py * widthPages + px;
                sum += g_g178PageGen[pg & (kG178PageCount - 1)].load(
                    std::memory_order_relaxed);
            }
        return sum;
    }
    if (g476PageRunOn())
    {
        // G476: same multiset of contributions, one load per distinct page (see g476RowWeight).
        for (uint32_t py = py0; py <= py1; ++py)
        {
            const uint32_t s = basePage + py * widthPages;
            for (uint32_t j = px0; j <= px1 + 1u; ++j)
                sum += static_cast<uint64_t>(g476RowWeight(j, px0, px1)) *
                       g_g178PageGen[(s + j) & (kG178PageCount - 1)].load(
                           std::memory_order_relaxed);
        }
        return sum;
    }
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

// G582: the page set a bump rect WOULD touch, as a 512-bit mask, without touching a generation.
// Used only by the G582 dedup oracle (DC2_G582_VERIFY=1) to assert the invariant that matters:
// the SET of pages whose generation moved must be identical with and without duplicate elision.
// Mirrors g178BumpRectImpl's walk exactly, including the conservative +1 slack page.
using G582PageMask = std::array<uint64_t, kG178PageCount / 64u>;

static void g582MarkRectPages(G582PageMask &mask, uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const G471PageRect pr = g471PageRect(bpBlocks, bw64, psm, x, y, w, h);
    const uint32_t slack = (g572ExactAlignedGenOn() && (bpBlocks & 31u) == 0u) ? 0u : 1u;
    for (uint32_t py = pr.py0; py <= pr.py1; ++py)
    {
        const uint32_t row = pr.basePage + py * pr.widthPages;
        for (uint32_t px = pr.px0; px <= pr.px1 + slack; ++px)
        {
            const uint32_t pg = (row + px) & (kG178PageCount - 1u);
            mask[pg >> 6u] |= 1ull << (pg & 63u);
        }
    }
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

// G570 diagnostic (default-off, DC2_G570_BUMP_TRACE=1): remember the last generation publisher
// that touched each physical GS page.  The G569-expanded RTT set packs several 512x416 CT32
// surfaces only a few pages apart; when the residency invariant moves, this identifies whether
// the writer was guest CPU traffic or publication of an aliased resident FBO.  The trace mirrors
// g178BumpRectImpl's exact page walk, including its conservative +1 slack page, but never changes
// generations or ownership state.
struct G570BumpTrace
{
    uint64_t seq = 0u;
    uint32_t source = 0u;
    uint32_t bpBlocks = 0u, bw64 = 0u, psm = 0u;
    uint32_t x = 0u, y = 0u, w = 0u, h = 0u;
};
static std::array<G570BumpTrace, kG178PageCount> g_g570LastBump{};
static std::atomic<uint64_t> g_g570BumpSeq{0u};

static bool g570BumpTraceOn()
{
    static const bool on = envFlagEnabled("DC2_G570_BUMP_TRACE");
    return on;
}

static void g570TraceBump(const G471PageRect &pr,
                          uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t source)
{
    if (!g570BumpTraceOn())
        return;
    const uint64_t seq = g_g570BumpSeq.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const G570BumpTrace trace{seq, source, bpBlocks, bw64, psm, x, y, w, h};
    const uint32_t slack =
        (g572ExactAlignedGenOn() && (bpBlocks & 31u) == 0u) ? 0u : 1u;
    for (uint32_t py = pr.py0; py <= pr.py1; ++py)
    {
        const uint32_t row = pr.basePage + py * pr.widthPages;
        for (uint32_t px = pr.px0; px <= pr.px1 + slack; ++px)
            g_g570LastBump[(row + px) & (kG178PageCount - 1u)] = trace;
    }
}

// ===== G582: page-generation BUMP census (diagnosis only, default OFF) =====
//
// The G446 PC sampler resolved against the linker /MAP puts `g178BumpRectImpl+0x5b5` — the
// `lock inc qword ptr [g_g178PageGen + rax*8]` inner loop — at 9.9% of EVERY GS-worker sample on
// the Ridepod boss, i.e. about half of all non-waiting in-exe samples on the pole thread. G476
// already refuted the "+1 slack page" as the cause and explicitly said to re-open the ATTRIBUTION
// instead; this is that census.
//
// Rank by VISITS (one `lock inc` each), never by calls: the call sites differ by two orders of
// magnitude in rect area, and a per-CALL count would rank a 1-page bbox bump equal to a 104-page
// whole-scissor one. Both columns are printed so `visits/call` is readable.
//
// Behaviour-pure: two relaxed atomic RMWs per call, and only when armed.
static bool g582BumpCensusOn()
{
    static const bool on = envFlagEnabled("DC2_G582_BUMPCENSUS");
    return on;
}

struct G582BumpBucket
{
    std::atomic<uint64_t> calls{0u};
    std::atomic<uint64_t> visits{0u};
};
static std::array<G582BumpBucket, 32> g_g582Bump{};
static std::atomic<uint64_t> g_g582BumpCalls{0u};

// Source tags. 0 stays "untagged" so a missed site is visible rather than silently merged.
static const char *const kG582BumpName[32] = {
    "untagged", "aliasfbpub", "cpurttcol", "cpurttdep", "flushcol", "flushdep",
    "inlinecol", "inlinedep", "l2ldst", "imgupload",
    "g260pubAcol", "g260pubAdep", "g260pubBcol", "g260pubBdep",
    "g286trncol", "g286trndep",
    "dispmirror", "src17", "aliasfull", "aliaslowA", "aliaslowB", "aliasz",
    "rttz", "matz", "matcolA", "matcolB",
    "src26", "src27", "src28", "src29", "src30", "other"};

// The G261/G272 alias publication site (source 1) packs its CAUSE into bits 8..15, so bucket on
// the low byte only -- otherwise every cause lands in "other".
static void g582NoteBump(uint32_t source, uint32_t rows, uint32_t cols)
{
    const uint32_t tag = source & 0xffu;
    const uint32_t idx = (tag < 32u) ? tag : 31u;
    g_g582Bump[idx].calls.fetch_add(1u, std::memory_order_relaxed);
    g_g582Bump[idx].visits.fetch_add(static_cast<uint64_t>(rows) * cols,
                                     std::memory_order_relaxed);
    const uint64_t n = g_g582BumpCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 200000ull) != 0ull)
        return;
    uint64_t totCalls = 0u, totVisits = 0u;
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        totCalls += g_g582Bump[i].calls.load(std::memory_order_relaxed);
        totVisits += g_g582Bump[i].visits.load(std::memory_order_relaxed);
    }
    std::fprintf(stderr, "[G582:bump] calls=%llu visits=%llu\n",
                 static_cast<unsigned long long>(totCalls),
                 static_cast<unsigned long long>(totVisits));
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        const uint64_t c = g_g582Bump[i].calls.load(std::memory_order_relaxed);
        if (c == 0u)
            continue;
        const uint64_t v = g_g582Bump[i].visits.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G582:bumpsrc] src=%u name=%-11s calls=%llu visits=%llu "
                     "perCall=%.1f shareVisits=%.1f%%\n",
                     i, kG582BumpName[i],
                     static_cast<unsigned long long>(c),
                     static_cast<unsigned long long>(v),
                     static_cast<double>(v) / static_cast<double>(c),
                     totVisits ? 100.0 * static_cast<double>(v) /
                                     static_cast<double>(totVisits) : 0.0);
    }
}

void g178BumpRectImpl(uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t g570Source = 0u)
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
    // G471: shift form of the identical page-rect prologue (see g471PageRect).
    const G471PageRect pr = g471PageRect(bpBlocks, bw64, psm, x, y, w, h);
    g471VerifyPageRect(pr, bpBlocks, bw64, psm, x, y, w, h);
    const uint32_t widthPages = pr.widthPages;
    const uint32_t py0 = pr.py0, py1 = pr.py1, px0 = pr.px0, px1 = pr.px1;
    const uint32_t basePage = pr.basePage;
    const bool exactAligned = g572ExactAlignedGenOn() && (bpBlocks & 31u) == 0u;
    if (g582BumpCensusOn())
        g582NoteBump(g570Source, py1 - py0 + 1u,
                     px1 - px0 + 1u + (exactAligned ? 0u : 1u));
    g570TraceBump(pr, bpBlocks, bw64, psm, x, y, w, h, g570Source);
    // G277-R mover probe: report every bump whose page walk (incl. the +1 slack) can touch the
    // 0x68 display chunk-12 pages 200..207 — pins the writer that trips publish-before-upload.
    {
        static const bool s_g277gm = envFlagEnabled("DC2_G277_GENMOVE");
        if (s_g277gm)
        {
            const uint32_t pgLo = basePage + py0 * widthPages + px0;
            const uint32_t pgHi = basePage + py1 * widthPages + px1 +
                                  (exactAligned ? 0u : 1u);
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
    if (exactAligned)
    {
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t pg = basePage + py * widthPages + px;
                g_g178PageGen[pg & (kG178PageCount - 1)].fetch_add(
                    1u, std::memory_order_relaxed);
            }
        return;
    }
    if (g476PageRunOn())
    {
        // G476: one `lock xadd` per DISTINCT page instead of one per (px, slack) visit. Weights
        // reproduce the old visit counts exactly — see g476RowWeight.
        if (g476VerifyOn())
        {
            // Oracle: tally both forms into plain per-page counters and compare the deltas. Runs
            // only under DC2_G476_VERIFY=1; the default path never allocates or touches this.
            static uint32_t refCount[kG178PageCount];
            static uint32_t newCount[kG178PageCount];
            for (uint32_t py = py0; py <= py1; ++py)
                for (uint32_t px = px0; px <= px1; ++px)
                {
                    const uint32_t pg = basePage + py * widthPages + px;
                    ++refCount[pg & (kG178PageCount - 1)];
                    ++refCount[(pg + 1) & (kG178PageCount - 1)];
                }
            for (uint32_t py = py0; py <= py1; ++py)
            {
                const uint32_t s = basePage + py * widthPages;
                for (uint32_t j = px0; j <= px1 + 1u; ++j)
                    newCount[(s + j) & (kG178PageCount - 1)] +=
                        g476RowWeight(j, px0, px1);
            }
            bool bad = false;
            for (uint32_t i = 0; i < kG178PageCount; ++i)
            {
                if (refCount[i] != newCount[i])
                    bad = true;
                refCount[i] = 0u;
                newCount[i] = 0u;
            }
            static std::atomic<uint64_t> s_checks{0};
            const uint64_t n = s_checks.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (bad)
            {
                static std::atomic<uint32_t> s_bad{0};
                if (s_bad.fetch_add(1u, std::memory_order_relaxed) < 32u)
                    std::fprintf(stderr,
                                 "[G476:pagerun-BAD] bp=0x%x bw=%u psm=0x%x rect=(%u,%u %ux%u) "
                                 "wp=%u px=%u..%u py=%u..%u\n",
                                 bpBlocks, bw64, psm, x, y, w, h, widthPages, px0, px1, py0, py1);
            }
            else
            {
                // Also tally the visits each form performs, so the lever's ceiling (atomic RMWs
                // removed per frame) is a measured number and not an inference from a PC sample.
                static std::atomic<uint64_t> s_refVisits{0}, s_newVisits{0};
                const uint64_t refV = static_cast<uint64_t>(py1 - py0 + 1u) *
                                      static_cast<uint64_t>(px1 - px0 + 1u) * 2ull;
                const uint64_t newV = static_cast<uint64_t>(py1 - py0 + 1u) *
                                      static_cast<uint64_t>(px1 - px0 + 2u);
                const uint64_t rv = s_refVisits.fetch_add(refV, std::memory_order_relaxed) + refV;
                const uint64_t nv = s_newVisits.fetch_add(newV, std::memory_order_relaxed) + newV;
                if (n <= 2ull || (n % 100000ull) == 0ull)
                    std::fprintf(stderr,
                                 "[G476:pagerun] walks=%llu bad=0 refVisits=%llu newVisits=%llu\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(rv),
                                 static_cast<unsigned long long>(nv));
            }
        }
        for (uint32_t py = py0; py <= py1; ++py)
        {
            const uint32_t s = basePage + py * widthPages;
            for (uint32_t j = px0; j <= px1 + 1u; ++j)
                g_g178PageGen[(s + j) & (kG178PageCount - 1)].fetch_add(
                    g476RowWeight(j, px0, px1), std::memory_order_relaxed);
        }
        return;
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
    // G513: which hash BODY produced `contentHash`. The 8-lane body is a different (equally strong,
    // not weaker) function of the same bytes, so its value differs from the reference body's. The
    // A/B instrument alternates the two bodies WITHIN one process, and a stamp written by one body
    // must never be compared against a digest computed by the other — that comparison is a
    // guaranteed MISS at best and, if the two ever collided, a STALE texture at worst. Recording
    // which body wrote the stamp turns an arm switch into one honest re-decode per key instead.
    uint8_t hashVar = 0xFFu;
};
std::unordered_map<uint64_t, G178TexReg> g_g178TexReg;

// ---------------------------------------------------------------------------------------------
// G513 — the source-page hash's byte loop.
//
// The REFERENCE body, verbatim from G178. Its emitted inner loop (dc2_runner.exe 0x1404104e4,
// read before this phase touched anything, per G505's law) is
//     xor rax, [rsi+rcx] ; add rcx,8 ; imul rax,r10 ; cmp ; jb
// i.e. a single loop-carried chain of xor(1c) -> imul(3c) = **4 host cycles per 8 bytes**, 0.5
// B/cycle. Nothing about that is bandwidth: ~9 pages = ~74 KB per call sits in L2, which sustains
// tens of bytes per cycle. The cost is the DEPENDENCY, and the lever is therefore ILP, not SIMD
// and not fewer bytes.
uint64_t g513HashMarkedRef(const uint8_t *vram, uint32_t vramSize, const bool *mark,
                           uint64_t *bytesOut)
{
    uint64_t bytes = 0u;
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
        bytes += 8192u;
    }
    if (bytesOut)
        *bytesOut = bytes;
    return hsh;
}

// The CANDIDATE body: eight INDEPENDENT FNV-1a chains, lane k taking the k-th 8-byte slot of every
// 64-byte group, carried across the whole marked page set in the same ascending page order, folded
// once at the end in a FIXED lane order.
//
// Why 8 and not 4 or 16: the binding resource is the 64-bit `imul` issue port (1/cycle) and its
// 3-cycle latency. Eight in flight issue in 8 cycles while each lane only needs its own result
// 4 cycles later, so 8 lanes exactly saturates the port at ~1 cycle per 8 bytes — the predicted
// 4x. More lanes buy nothing (the port is already full) and start spilling; the memcpy loads fold
// into the xor operand, so the lanes cost 8 GPRs and no scratch.
//
// EXACTNESS. This is a DIFFERENT digest of the SAME bytes, not the same digest computed faster —
// FNV-1a's xor step is not affine over the multiply's ring, so it cannot be split without changing
// the value. That is admissible here only because the digest is never compared against anything
// but a stamp this same body wrote (see G178TexReg::hashVar), and because it is not WEAKER: every
// byte still passes through a full 64-bit FNV chain, the folded state is 512 bits reduced to 64,
// and the lanes are seeded distinctly so a permutation of the data between lane positions cannot
// cancel. The false-HIT risk the roadmap flags is a STRENGTH question, and 8x64 folded to 64 is
// not less discriminating than 1x64. `DC2_G513_VERIFY=1` tests exactly that claim at runtime, by
// running both bodies and comparing the two hit/miss DECISIONS rather than the two values.
uint64_t g513HashMarkedFast(const uint8_t *vram, uint32_t vramSize, const bool *mark,
                            uint64_t *bytesOut)
{
    constexpr uint64_t kP = 1099511628211ull;
    constexpr uint64_t kBasis = 1469598103934665603ull;
    constexpr uint64_t kLaneSalt = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = kBasis ^ (0ull * kLaneSalt), h1 = kBasis ^ (1ull * kLaneSalt);
    uint64_t h2 = kBasis ^ (2ull * kLaneSalt), h3 = kBasis ^ (3ull * kLaneSalt);
    uint64_t h4 = kBasis ^ (4ull * kLaneSalt), h5 = kBasis ^ (5ull * kLaneSalt);
    uint64_t h6 = kBasis ^ (6ull * kLaneSalt), h7 = kBasis ^ (7ull * kLaneSalt);
    uint64_t bytes = 0u;
    for (uint32_t pg = 0; pg < kG178PageCount; ++pg)
    {
        if (!mark[pg])
            continue;
        const size_t off = static_cast<size_t>(pg) * 8192u;
        if (off + 8192u > vramSize)
            continue;
        const uint8_t *p = vram + off;
        // 8192 is an exact multiple of 64, so there is no tail and no tail branch.
        for (size_t i = 0; i < 8192u; i += 64)
        {
            uint64_t v0, v1, v2, v3, v4, v5, v6, v7;
            std::memcpy(&v0, p + i + 0, 8);
            std::memcpy(&v1, p + i + 8, 8);
            std::memcpy(&v2, p + i + 16, 8);
            std::memcpy(&v3, p + i + 24, 8);
            std::memcpy(&v4, p + i + 32, 8);
            std::memcpy(&v5, p + i + 40, 8);
            std::memcpy(&v6, p + i + 48, 8);
            std::memcpy(&v7, p + i + 56, 8);
            h0 = (h0 ^ v0) * kP;
            h1 = (h1 ^ v1) * kP;
            h2 = (h2 ^ v2) * kP;
            h3 = (h3 ^ v3) * kP;
            h4 = (h4 ^ v4) * kP;
            h5 = (h5 ^ v5) * kP;
            h6 = (h6 ^ v6) * kP;
            h7 = (h7 ^ v7) * kP;
        }
        bytes += 8192u;
    }
    uint64_t hsh = kBasis;
    hsh = (hsh ^ h0) * kP;
    hsh = (hsh ^ h1) * kP;
    hsh = (hsh ^ h2) * kP;
    hsh = (hsh ^ h3) * kP;
    hsh = (hsh ^ h4) * kP;
    hsh = (hsh ^ h5) * kP;
    hsh = (hsh ^ h6) * kP;
    hsh = (hsh ^ h7) * kP;
    if (bytesOut)
        *bytesOut = bytes;
    return hsh;
}

// Collect the distinct VRAM page indices a texture's conservative range covers (same walk as
// g178GenSumRect, deduped), then FNV-hash those pages' raw bytes. ~8-32 pages for title textures
// — a sequential ~64-256KB hash, far cheaper than a swizzle+CLUT whole-texture decode.
// G513: `fast` selects the byte loop; the PAGE SET and the page ORDER are identical either way, so
// the only variable between the two arms is the inner loop's dependency structure.
uint64_t g178HashPages(const uint8_t *vram, uint32_t vramSize,
                       uint32_t bpBlocks, uint32_t bw64, uint32_t psm,
                       uint32_t w, uint32_t h,
                       uint32_t clutBpBlocks, bool fast, uint64_t *bytesOut)
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

    if (bytesOut)
        *bytesOut = 0u;
    return fast ? g513HashMarkedFast(vram, vramSize, mark, bytesOut)
                : g513HashMarkedRef(vram, vramSize, mark, bytesOut);
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
                         static_cast<uint32_t>(rows),
                         16u); // G582 source 16: display-mirror publication
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

#include "g512_fast_texdecode.inc"

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

    // ------------------------------------------------------------------------------------------
    // G512: build the fast path's plan ONCE per decode (4 calls/frame), never per lane and never
    // per texel. Everything below is loop-invariant for the whole texture; the reference body
    // re-derived all of it 81,601 times through closure pointers. See g512_fast_texdecode.inc.
    // ------------------------------------------------------------------------------------------
    // DC2_G365_WATCH puts a per-texel census inside readTexelPSMCT32; the fast path does not call
    // it, so that diagnostic keeps the reference body (it is default-off on every perf run).
    static const bool s_g365Watch = (std::getenv("DC2_G365_WATCH") != nullptr);
    G512DecodePlan plan;
    alignas(64) uint32_t g512Clut[256];
    const bool g512Fast = g512FastDecActive() && !s_g365Watch && g512PsmFastable(psm) &&
                          texW > 0 && texW <= kG512MaxRowW && texH > 0;
    if (g512Fast)
    {
        plan.usable = true;
        plan.psm = psm;
        plan.tbp0 = ctx.tex0.tbp0;
        plan.bwDirect = (ctx.tex0.tbw == 0u) ? 1u : ctx.tex0.tbw; // readTexelPSMCT32's rule
        plan.bwT8 = pageBwT8;
        plan.bwT4 = ((ctx.tex0.tbw >> 1u) != 0u) ? (ctx.tex0.tbw >> 1u) : 1u;
        plan.bwT4High = ctx.tex0.tbw;
        plan.ta0 = texa.ta0;
        plan.aem = texa.aem != 0u;
        if (g512PsmUsesClut(psm))
        {
            g512BuildClut(vram, texa, texclut, ctx, psm, g512Clut);
            plan.clut = g512Clut;
        }
    }

    const auto joinedDecode = [&](int rowLo, int rowHi) {
        if (plan.usable)
            g512DecodeRowsFast(vram, plan, texW, rowLo, rowHi, outPx);
        else
            decodeRows(rowLo, rowHi, outPx);
    };
    GSRowPool::instance().run(0, texH - 1, lanes, joinedDecode);

    // G512 census (DC2_G512_PSM=1). Behaviour-pure, counted on both arms and on the default path.
    if (g512PsmCensusOn())
        g512PsmNote(psm, static_cast<uint64_t>(pixels), g512Fast, texW, texH, lanes);

    // G512 exactness oracle (DC2_G512_VERIFY=1): decode the SAME texture with the pre-G512 body
    // and count mismatching texels. This compares fast-vs-reference, which is what has to hold;
    // DC2_G298_VERIFY_TEXDECODE below only compares the parallel decode against the same body.
    if (g512Fast && g512VerifyOn())
    {
        static thread_local std::vector<uint32_t> s_g512Ref;
        s_g512Ref.resize(static_cast<size_t>(texW) * texH);
        decodeRows(0, texH - 1, s_g512Ref.data());
        uint64_t bad = 0u;
        for (size_t i = 0; i < s_g512Ref.size(); ++i)
            bad += outPx[i] != s_g512Ref[i] ? 1u : 0u;
        static std::atomic<uint64_t> s_n{0u}, s_px{0u}, s_bad{0u};
        const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t totPx =
            s_px.fetch_add(static_cast<uint64_t>(pixels), std::memory_order_relaxed) +
            static_cast<uint64_t>(pixels);
        const uint64_t totBad = s_bad.fetch_add(bad, std::memory_order_relaxed) + bad;
        if (bad != 0u || n <= 8u || (n % 256u) == 0u)
            std::fprintf(stderr,
                         "[G512:verify] calls=%llu px=%llu bad=%llu last=%dx%d psm=%02x "
                         "cpsm=%02x lastBad=%llu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(totPx),
                         static_cast<unsigned long long>(totBad), texW, texH,
                         static_cast<unsigned>(psm), static_cast<unsigned>(ctx.tex0.cpsm),
                         static_cast<unsigned long long>(bad));
    }

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
    uint8_t fge = 0;      // G364
    uint32_t fogCol = 0;  // G364
    uint8_t depthFunc = 0;
    bool depthWrite = false, bilinear = false;
    uint8_t wrapU = 0, wrapV = 0;
    uint16_t scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
    uint16_t zbpPage = 0;
    uint8_t zpsm = 0;
    bool guestDepth = false;
    bool skip = false; // valid but draws nothing (e.g. ZTST=NEVER)
};
