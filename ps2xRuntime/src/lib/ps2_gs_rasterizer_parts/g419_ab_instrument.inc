// G419 — generalized within-process randomized A/B instrument + the exact CT32 upload fast pack.
//
// WHY THIS EXISTS. G418 proved that separate-process lean pairs on this route carry a +-1.5 ms
// run-level offset, which cannot adjudicate a sub-millisecond mechanism: five alternating pairs
// reported the two arms of the SAME mechanism with opposite signs. That is what forced G417 to
// retire an exact optimization it could not separate, and what made G414/G415 shelve exact levers as
// "below the timing floor". G418's fix was an instrument bolted onto one call site. This part lifts
// it into a lever-agnostic instrument so any shelved lever can be re-adjudicated:
//
//   * ONE fixed sample point per frame. Exactly one colour-readback unpack happens per frame, so the
//     interval between consecutive unpacks IS a frame period sampled at a fixed point in the frame.
//     g419AbTick() is called there and nowhere else.
//   * THE ARM IS CHOSEN AT THE TICK, but CHARGED BY THE CONSUMER. A consumer records which arm it
//     actually used (g_g419AbUsed); the next tick charges the elapsed interval to that arm and
//     clears the record. An interval with no consumer, or with both arms observed, is dropped. This
//     makes attribution correct no matter which thread hosts the lever or how deep the host pipeline
//     is - the period is charged to the arm that demonstrably served it. G418's design charged the
//     arm it PICKED, which is only equivalent for a same-thread, zero-lag consumer.
//   * PER-FRAME RANDOMISATION, not block alternation. Fixed-parity block alternation aliases with the
//     route's own periodicity: G418 measured its promoted arm as 0.79 ms SLOWER at block=60 and the
//     sign flipped under parity inversion. The arm comes from a fixed LCG, which cannot alias.
//   * WARM-UP SKIP. Whichever bucket owns the first frames inherits the route's load-in cost.
//   * BLOCKED ESTIMATOR IN-PROCESS. Both arm means drift together, so the difference of per-window
//     means is far tighter than the pooled SE (G418: 0.077 vs 0.120 ms). G418 computed this by hand
//     from the periodic prints; it is now printed directly, with its t statistic.
//   * BALLAST. Both buckets run the CONTROL form and one bucket repeats it N extra times
//     (idempotent). measured_delta / known_added_cost is the site's critical-path sensitivity.
//     Multiply an isolated saving by it BEFORE predicting a payoff - this is the step G414's and
//     G417's inclusive-timer deletions were missing.
//
// Diagnostic-only: it deliberately runs both arms, so DC2_G419_AB is never a shipping mode.
//
// -----------------------------------------------------------------------------------------------
// THE EXACT CT32 UPLOAD FAST PACK (the lever this phase re-adjudicates on the upload side).
//
// The framebuffer-upload staging pack is the mirror image of G418's readback unpack: for every
// guest row it walks x with a full addrPSMCT32() call plus a 4-byte memcpy out of VRAM. G418's
// decomposition applies unchanged, because it is a property of addrPSMCT32 and not of a direction:
//
//     addr(x,y) == (((block>>5) + (y>>5)*width) << 13) + ((x>>6) << 13) + tbl[y&31][x&63]
//
// with tbl[yl][xl] := addrPSMCT32(block, width, xl, yl) - ((block>>5)<<13). The only x term above
// bit 5 is the linear (x>>6) page step; the only y terms are the linear (y>>5) page step and bits
// 0..4 feeding blockTable32/columnTable32, all of which the 32x64 table covers exhaustively. The
// table is literally the function restricted to one page, so nothing is approximated - including a
// non-zero (block&31), whose page carry is already baked into every table entry.
//
// PAIRED LOADS. columnTable32[y&7][x&7] for an even x and x+1 differ by exactly one word and share
// (x>>3)/(x>>6), so the two source pixels are contiguous in VRAM: an even-width row reads as 8-byte
// loads. The partial-bound cases reproduce the original per-pixel behaviour exactly (see below).
//
// WHY A MULTI-ENTRY TABLE CACHE. G418's single-entry cache is correct for the readback, which only
// ever sees fbp=0x13b at one width. Uploads visit fbp=0x139 at BOTH fbw=8 and fbw=2 plus 0x0, 0x68
// and several singletons in the same frame, so a single-entry cache would rebuild 2048 addresses per
// upload and eat the win. A small direct cache keyed by (block,width) makes every steady-state
// lookup a hit. Flush-owner thread only, exactly like G418's - the lanes only read it, after
// GSRowPool::run has published it through the dispatch.

// The shared arm register (g_g419AbArm / g_g419AbUsed) is defined in
// ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc and declared at global scope in
// rasterizer_headers_and_diagnostics.inc:
//   g_g419AbArm  : 0 = control, 1 = candidate. Written only by the tick.
//   g_g419AbUsed : bit0 = a consumer ran the control form, bit1 = the candidate form. Written by
//                  consumers, cleared by the tick.

enum G419Lever
{
    kG419LeverNone = 0,
    kG419LeverNarrow = 1, // DC2_G415_NARROW_READBACK  (GL backend, colour readback rows)
    kG419LeverPack = 2,   // the exact CT32 upload pack (GS front-end, this file)
    kG419LeverLanes = 3,  // pixel-gated row-pool fan-out for the upload pack (this file)
    kG419LeverAll = 4,    // G419's three together == the G419 shipping stack (its promotion gate)
    // G420 levers. `all420` is their combined arm; `all` deliberately still means G419's stack, so
    // the two phases' promotion gates never blur into one another.
    kG420LeverZero = 5,   // exact staging zero-fill elision (g420_pack_residue.inc)
    kG420LeverPLanes = 6, // pack row-pool LANE COUNT (alt vs G298's 8)
    kG420LeverPool = 7,   // GSRowPool dispatch narrowing (wait only for lanes that took work)
    kG420LeverAll = 8,    // the three G420 BEHAVIOUR levers (narrowtouch is diagnostic, excluded)
    // MECHANISM PROBE, never a behaviour lever and never in a combined arm: the candidate bucket
    // writes the complement rows the FULL readback would have written, into an inert scratch
    // buffer. It adds the full read's destination-side memory traffic WITHOUT its GL transfer, so
    // it measures the leading hypothesis for G419's unaccounted narrow-readback payoff directly.
    kG420LeverNTouch = 9,
    // G432: the exact G418 colour-readback unpack applied at the SECOND publication site
    // (g261Materialize, rasterizer_gpu_alias_page_view.inc). Sub-millisecond, so the within-process
    // randomized A/B is its only admissible gate. The site references this id by value because it is
    // compiled earlier in this TU than this enum (see kG432MatUnpackLeverId there).
    kG432LeverMatUnpack = 10,
    // G438: the two DERIVATIVE PROBES moved inside the within-process instrument. They are not
    // behaviour levers and can never be promoted — the candidate arm deliberately WASTES time on
    // one thread. The point is that G433's replication rule (the RUN is the unit, and the run-level
    // frame mean moves 44.8–48.6 ms) makes a cross-run derivative spin unusable on a quiet day:
    // G438 block A read VU1 +1.96 / GS −2.45 and block B read VU1 −0.83 / GS −0.27, i.e. both
    // probes disagreed with themselves in SIGN. Alternating the spin per FRAME inside one process
    // differences the drift away exactly as it does for a sub-ms behaviour lever, and turns the
    // pole ranking into a paired t statistic instead of a difference of two noisy run means.
    kG438LeverVu1Slow = 11, // spin DC2_G303_VU1_SLOW_US per kick on candidate frames only
    kG438LeverGsSlow = 12,  // spin DC2_G431_GS_SLOW_US per window on candidate frames only
    // G439: the WINDOW-COUNT ceiling probe. G434 priced every named block of work on the GS worker
    // at 2.83 ms/f while G438 measured the frame absorbing ~96% of any microsecond added PER WINDOW,
    // so what the thread transmits is the 1,596-windows/frame handoff chain, not the work inside it.
    // The candidate arm merges DC2_G439_COALESCE consecutive drain() windows into one queue item:
    // window count drops ~N x while kicks/frame and packets/frame are exactly conserved (the
    // workload invariant). Knowingly INCORRECT output — processWindow's stable path sort now spans
    // what were separate windows — so this is a ceiling probe and can never be promoted.
    kG439LeverCoalesce = 13,
    // G445: the G418 early command kick (one glFlush per skip-readback render batch), moved into the
    // within-process instrument. It is a real BEHAVIOUR lever and exact by construction — glFlush
    // changes no result, no GL state, no command order and no ownership — but its only prior gate was
    // a cross-run block taken while VU1 bound the frame, i.e. the G444 expiry law applies to it. On
    // this host the run-level frame mean drifts ~1.4x within a session (46.6 -> 33.3 on the base arm
    // in one G445 block), which is several times the lever, so a cross-run A/B cannot gate it.
    // Thread-level (it changes when the GL backend starts executing queued work, and the effect lands
    // on the NEXT batch's drain), so it inherits the block hold like the two spins and coalesce.
    kG445LeverEarlyKick = 14,
    // G445 extension probe: the same kick applied to the backend items G418 never covered — the
    // GPU->GPU alias copies and the decode/composite/upload class ("copy" and "other" in
    // [G332:gsw]). Those leave GL work queued and never fence either, so on the same argument they
    // should also be contributing to the readback's drain. Separate lever id because it is a second
    // architectural decision (WHICH items are kicked), not a re-measurement of the first.
    kG445LeverKickAll = 15,
    // G447: spin-then-block on the GL backend round trip. G447's edge census proved 100% of the
    // pole thread's 16.7 ms/f of blocked time is these ~74 promise/future round trips per frame, and
    // that the GL worker's own work only accounts for ~13 of it. Thread-level (it changes when the
    // GS worker resumes after a handoff), so it inherits the block hold.
    kG447LeverSpinWait = 16,
    // G447: the SUBMIT half of the same round trip — the GL backend worker spins on a submit
    // sequence counter before sleeping, so a queued item does not wait for that thread to be
    // scheduled back on. Separate id: it is a different thread burning the CPU.
    kG447LeverWSpin = 17,
    // G447 PROMOTION GATE: both halves of the round trip alternated together, i.e. exactly the
    // shipped default against exactly the double rollback. The two levers act on DIFFERENT threads
    // and each carries a CPU-burn cost, so only a combined arm answers "does the promoted pair pay".
    kG447LeverSpinBoth = 18,
};

// DC2_G419_AB=<lever> selects the ONE lever the instrument alternates. Exactly one at a time:
// a phase must keep one architectural variable per arm, and a shared arm register cannot attribute
// two levers from one period series.
// G447 HOST CHECK (DC2_G447_HOSTCHECK=1). One flag that turns a run on an UNFAMILIAR MACHINE into a
// copy-pasteable verdict on the two promoted G447 spins — the arc's only host-shaped levers, since
// they buy wall time by burning CPU on an otherwise-blocked thread. It selects the combined
// `spinboth` arm (the promotion gate: exactly the shipped default against exactly the full
// rollback), reports the host's core count, and prints a plain verdict beside every `[G419:ab]`
// window. An explicit DC2_G419_AB still wins, so this can never silently hijack another phase's A/B.
static bool g447HostCheckOn()
{
    static const bool on = (std::getenv("DC2_G447_HOSTCHECK") != nullptr);
    return on;
}

static int g419AbLever()
{
    static const int lever = [] {
        const char *value = std::getenv("DC2_G419_AB");
        if (value == nullptr || value[0] == '\0')
            return g447HostCheckOn() ? static_cast<int>(kG447LeverSpinBoth)
                                     : static_cast<int>(kG419LeverNone);
        if (std::strcmp(value, "narrow") == 0)
            return static_cast<int>(kG419LeverNarrow);
        if (std::strcmp(value, "pack") == 0)
            return static_cast<int>(kG419LeverPack);
        if (std::strcmp(value, "lanes") == 0)
            return static_cast<int>(kG419LeverLanes);
        if (std::strcmp(value, "all") == 0)
            return static_cast<int>(kG419LeverAll);
        if (std::strcmp(value, "zero") == 0)
            return static_cast<int>(kG420LeverZero);
        if (std::strcmp(value, "packlanes") == 0)
            return static_cast<int>(kG420LeverPLanes);
        if (std::strcmp(value, "pool") == 0)
            return static_cast<int>(kG420LeverPool);
        if (std::strcmp(value, "all420") == 0)
            return static_cast<int>(kG420LeverAll);
        if (std::strcmp(value, "narrowtouch") == 0)
            return static_cast<int>(kG420LeverNTouch);
        if (std::strcmp(value, "matunpack") == 0)
            return static_cast<int>(kG432LeverMatUnpack);
        if (std::strcmp(value, "vu1slow") == 0)
            return static_cast<int>(kG438LeverVu1Slow);
        if (std::strcmp(value, "gsslow") == 0)
            return static_cast<int>(kG438LeverGsSlow);
        if (std::strcmp(value, "coalesce") == 0)
            return static_cast<int>(kG439LeverCoalesce);
        if (std::strcmp(value, "earlykick") == 0)
            return static_cast<int>(kG445LeverEarlyKick);
        if (std::strcmp(value, "kickall") == 0)
            return static_cast<int>(kG445LeverKickAll);
        if (std::strcmp(value, "spinwait") == 0)
            return static_cast<int>(kG447LeverSpinWait);
        if (std::strcmp(value, "wspin") == 0)
            return static_cast<int>(kG447LeverWSpin);
        if (std::strcmp(value, "spinboth") == 0)
            return static_cast<int>(kG447LeverSpinBoth);
        std::fprintf(stderr,
                     "[G419:ab] unknown DC2_G419_AB='%s' (expected "
                     "narrow|pack|lanes|all|zero|packlanes|pool|all420|narrowtouch|matunpack|"
                     "vu1slow|gsslow|coalesce|earlykick|kickall|spinwait|wspin|spinboth)\n",
                     value);
        return static_cast<int>(kG419LeverNone);
    }();
    return lever;
}

static const char *g419AbLeverName(int lever)
{
    switch (lever)
    {
    case kG419LeverNarrow: return "narrow";
    case kG419LeverPack: return "pack";
    case kG419LeverLanes: return "lanes";
    case kG419LeverAll: return "all";
    case kG420LeverZero: return "zero";
    case kG420LeverPLanes: return "packlanes";
    case kG420LeverPool: return "pool";
    case kG420LeverAll: return "all420";
    case kG420LeverNTouch: return "narrowtouch";
    case kG432LeverMatUnpack: return "matunpack";
    case kG438LeverVu1Slow: return "vu1slow";
    case kG438LeverGsSlow: return "gsslow";
    case kG439LeverCoalesce: return "coalesce";
    case kG445LeverEarlyKick: return "earlykick";
    case kG445LeverKickAll: return "kickall";
    case kG447LeverSpinWait: return "spinwait";
    case kG447LeverWSpin: return "wspin";
    case kG447LeverSpinBoth: return "spinboth";
    default: return "none";
    }
}

// DC2_G419_BALLAST=<repeats> — critical-path sensitivity probe. BOTH buckets run the control form;
// the candidate bucket additionally repeats the site's work <repeats> times. The repeat must be
// idempotent (it rewrites identical bytes), so the only thing measured is how much of a KNOWN added
// cost at that exact site reaches the frame.
static int g419AbBallast()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G419_BALLAST");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(16, std::atoi(value)));
    }();
    return n;
}

static uint64_t g419AbWarmFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WARM");
        if (value == nullptr)
            return 120;
        return std::max(0, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Blocked-estimator window length in ticks. Both arm means drift together over a run, so a window
// short enough that the drift is common to both buckets differences it away.
static uint64_t g419AbWindow()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WINDOW");
        if (value == nullptr)
            return 600;
        return std::max(60, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Consumer-side arm selector. Returns the behaviour this call should use and records which bucket
// actually ran so the next tick can charge the interval to it.
//   * not the selected lever -> the promoted default, and nothing is recorded (zero instrument cost
//     on the shipping path beyond one cached-int compare);
//   * ballast mode -> the CONTROL form for both buckets (the extra work is added by the site).
// `all` alternates every lever at once, which is the promotion gate for the shipping stack: the three
// levers act on different threads, so only a combined arm answers "does the promoted default pay".
static bool g419AbLeverMatches(int lever)
{
    const int sel = g419AbLever();
    if (sel == lever)
        return true;
    // The two combined arms are each restricted to their OWN phase's levers. `all` must keep
    // meaning "the G419 shipping stack" so its promotion gate stays reproducible.
    if (sel == kG419LeverAll)
        return lever >= kG419LeverNarrow && lever <= kG419LeverLanes;
    if (sel == kG420LeverAll)
        return lever >= kG420LeverZero && lever <= kG420LeverPool;
    return false;
}

static bool g419AbArmFor(int lever, bool defaultOn)
{
    if (!g419AbLeverMatches(lever))
        return defaultOn;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    if (g419AbBallast() > 0)
        return false;
    return arm != 0u;
}

// G438: the derivative probes' arm selector, callable from the VU1-worker and GS-worker TUs (the
// rest of this instrument is static to the rasterizer TU). `which` is 0 for the VU1 spin, 1 for the
// GS spin. Returns -1 when that probe is not the selected lever (the caller then falls back to the
// legacy always-on DC2_G303_VU1_SLOW_US / DC2_G431_GS_SLOW_US behaviour), 0 on a control frame and
// 1 on a candidate frame.
//
// The `used` bit must reach the tick, but the spin sites run per-KICK and per-WINDOW (~1250 and
// ~1580 times/frame), and an unconditional atomic RMW on a line shared with the EE thread would be
// a per-call cross-core cost paid by BOTH arms — i.e. it would inflate the very baseline the probe
// is dividing by. The arm only changes once per frame, so a thread-local memo turns it into one
// atomic per thread per frame.
// G438: how many consecutive frames one arm is held for. 1 (the default, and the only value the
// G419/G420/G432 behaviour levers ever use) reproduces the original per-frame alternation exactly.
// DC2_G438_HOLD sets it; the derivative probes default to 16.
static uint64_t g438ArmHoldFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G438_HOLD");
        if (value != nullptr && value[0] != '\0')
            return static_cast<uint64_t>(std::max(1, std::min(600, std::atoi(value))));
        const int sel = g419AbLever();
        // G439's coalesce probe is a thread-level lever like the two spins (it changes the handoff
        // rate between the EE and the GS worker), so it inherits the block hold for the same
        // pipelined-runtime reason — see the g419AbTick comment on DC2_G438_HOLD.
        return (sel == kG438LeverVu1Slow || sel == kG438LeverGsSlow ||
                sel == kG439LeverCoalesce || sel == kG445LeverEarlyKick ||
                sel == kG445LeverKickAll || sel == kG447LeverSpinWait || sel == kG447LeverWSpin ||
                sel == kG447LeverSpinBoth)
                   ? 16ull
                   : 1ull;
    }();
    return n;
}

// Bumped once per frame by g419AbTick. The memo keys on this, NOT on the arm value: the LCG draws
// the same arm on consecutive frames about half the time, and keying on the value would then leave
// `used` clear and the tick would drop that frame as unattributable.
static std::atomic<uint64_t> s_g438TickSeq{0ull};

bool g438SlowLeverSelected(int which);
int g438SlowArm(int which)
{
    const int want = (which == 0) ? static_cast<int>(kG438LeverVu1Slow)
                                  : static_cast<int>(kG438LeverGsSlow);
    if (g419AbLever() != want)
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

bool g438SlowLeverSelected(int which)
{
    const int want = (which == 0) ? static_cast<int>(kG438LeverVu1Slow)
                                  : static_cast<int>(kG438LeverGsSlow);
    return g419AbLever() == want;
}

// G439: the window-coalescing ceiling probe's arm selector, called from ps2_gif_arbiter.cpp on the
// EE thread. Same contract and same per-frame memo as g438SlowArm: -1 when the probe is not the
// selected lever (the site then honours the always-on DC2_G439_COALESCE value), 0 on a control
// frame, 1 on a candidate frame. The call site is GifArbiter::drain(), ~1,600 times per frame, so
// the `used` bit must not be an unconditional cross-core RMW.
int g439CoalesceArm()
{
    if (g419AbLever() != static_cast<int>(kG439LeverCoalesce))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G445: the G418 early-command-kick arm selector, called from the GL backend worker TU
// (lle_gpu_raster_backend.inc). Same contract and same per-frame memo as g438SlowArm: -1 when the
// probe is not the selected lever (the site then honours the always-on DC2_G418_EARLY_KICK value),
// 0 on a control frame, 1 on a candidate frame. The site runs once per skip-readback render batch
// (~33/frame), so the `used` bit must not be an unconditional cross-core RMW.
int g445KickArm()
{
    if (g419AbLever() != static_cast<int>(kG445LeverEarlyKick))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G447 spin-then-block arm selector. Same contract and same per-frame memo as g445KickArm; the site
// is every synchronous backend entry point on the GS worker (~74 items/frame).
int g447SpinArm()
{
    const int sel447 = g419AbLever();
    if (sel447 != static_cast<int>(kG447LeverSpinWait) &&
        sel447 != static_cast<int>(kG447LeverSpinBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G447 worker-side spin arm selector. Same contract and same per-frame memo as g447SpinArm; the
// site is the GL backend worker's pre-sleep path (~74 sleeps/frame).
int g447WorkerSpinArm()
{
    const int sel447w = g419AbLever();
    if (sel447w != static_cast<int>(kG447LeverWSpin) &&
        sel447w != static_cast<int>(kG447LeverSpinBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G445 extension probe arm selector. Same contract and same per-frame memo as g445KickArm; the site
// is the GL backend worker loop, once per copy/composite/upload item (~33/frame).
int g445KickAllArm()
{
    if (g419AbLever() != static_cast<int>(kG445LeverKickAll))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// How many extra idempotent repetitions this frame's site should perform (ballast mode only).
static int g419AbBallastReps(int lever)
{
    const int n = g419AbBallast();
    if (n <= 0 || !g419AbLeverMatches(lever))
        return 0;
    return g_g419AbArm.load(std::memory_order_relaxed) != 0u ? n : 0;
}

// Frames observed at the instrument's sample point. Counted unconditionally (one increment per
// frame) so every G419 per-batch census can be normalised to a real per-frame figure. Guessing the
// divisor is how a per-batch cost turns into a wrong payoff prediction: DC2 issues ~1.7 uploads of
// fbp=0x139 per frame, not one, so a plausible-looking "one per frame" assumption was off by 70%.
static uint64_t s_g419FrameTicks = 0u;

static void g419NoteFrameTick()
{
    ++s_g419FrameTicks;
}

// Once-per-frame tick at the colour-readback unpack. Charges the elapsed interval to whichever arm a
// consumer reported, then draws the next frame's arm.
static void g419AbTick()
{
    if (g419AbLever() == kG419LeverNone)
        return;
    static std::chrono::steady_clock::time_point s_last{};
    static bool s_haveLast = false;
    static uint64_t s_frame = 0u;
    static uint64_t s_lcg = 0x9E3779B97F4A7C15ull;
    // [0] = candidate, [1] = control.
    static double s_sum[2]{}, s_sq[2]{};
    static uint64_t s_n[2]{};
    static double s_winSum[2]{};
    static uint64_t s_winN[2]{};
    static double s_blkSum = 0.0, s_blkSq = 0.0;
    static uint64_t s_blkN = 0u, s_blkFav = 0u, s_dropped = 0u;
    // G438: frames whose interval spans an arm CHANGE. See g438ArmHoldFrames below.
    static bool s_skipNext = false;
    static uint64_t s_transition = 0u;

    const auto now = std::chrono::steady_clock::now();
    const uint32_t used = g_g419AbUsed.exchange(0u, std::memory_order_relaxed);
    const uint64_t idx = s_frame++;
    const bool skipThis = s_skipNext;
    s_skipNext = false;
    if (skipThis)
        ++s_transition;
    if (s_haveLast && !skipThis && idx >= g419AbWarmFrames())
    {
        const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
        // An interval with no consumer (used==0) or with both arms observed (used==3) cannot be
        // attributed. Frame periods are wall-clock, so clip the disc-read / transition outliers that
        // no mean survives (same 200 ms clip G418 used against this route's ~66 ms steady frame).
        if ((used == 1u || used == 2u) && ms < 200.0)
        {
            const int a = (used == 2u) ? 0 : 1;
            s_sum[a] += ms;
            s_sq[a] += ms * ms;
            ++s_n[a];
            s_winSum[a] += ms;
            ++s_winN[a];
        }
        else
        {
            ++s_dropped;
        }
    }
    s_last = now;
    s_haveLast = true;

    // G438: hold the arm for a block of frames instead of redrawing every frame. A per-frame draw
    // is right for a lever whose whole cost lands inside the frame that ran it, but this runtime
    // pipelines frames across threads (G412 one-successor cross-frame pipelining), so work injected
    // on a candidate frame partly lands in the NEXT frame period. With alternation every frame that
    // successor is a control frame half the time, which drains the measured difference towards zero:
    // the vu1slow probe read +0.21 ms for 6.25 ms/f of injected spin (sensitivity 0.03) against
    // 0.44-0.98 from the cross-run form of the same probe. Holding the arm for N frames confines
    // the smear to the ONE boundary frame per block, which is then dropped outright.
    const uint64_t hold = g438ArmHoldFrames();
    if (hold <= 1u || (idx % hold) == 0u)
    {
        s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t next = ((s_lcg >> 33u) & 1ull) != 0ull ? 1u : 0u;
        if (hold > 1u && next != g_g419AbArm.load(std::memory_order_relaxed))
            s_skipNext = true;
        g_g419AbArm.store(next, std::memory_order_relaxed);
    }
    // Publish the frame identity AFTER the arm, so a worker that observes the new seq also observes
    // the arm that belongs to it.
    s_g438TickSeq.fetch_add(1ull, std::memory_order_relaxed);

    const uint64_t window = g419AbWindow();
    if (idx == 0u || (idx % window) != 0u)
        return;
    // Close the blocked window. Require a real paired sample count in both buckets so a window the
    // route spent loading or stalled in cannot contribute a spurious difference.
    if (s_winN[0] >= 8u && s_winN[1] >= 8u)
    {
        const double d = s_winSum[0] / static_cast<double>(s_winN[0]) -
                         s_winSum[1] / static_cast<double>(s_winN[1]);
        s_blkSum += d;
        s_blkSq += d * d;
        ++s_blkN;
        if (d < 0.0)
            ++s_blkFav;
    }
    s_winSum[0] = s_winSum[1] = 0.0;
    s_winN[0] = s_winN[1] = 0u;

    const auto mean = [](double sum, uint64_t n) {
        return n != 0u ? sum / static_cast<double>(n) : 0.0;
    };
    const double c = mean(s_sum[0], s_n[0]), k = mean(s_sum[1], s_n[1]);
    const auto sem = [&](int a, double m) {
        const uint64_t n = s_n[a];
        if (n < 2u)
            return 0.0;
        const double var =
            (s_sq[a] - static_cast<double>(n) * m * m) / static_cast<double>(n - 1u);
        return var > 0.0 ? std::sqrt(var / static_cast<double>(n)) : 0.0;
    };
    const double sc = sem(0, c), sk = sem(1, k);
    double blkMean = 0.0, blkSe = 0.0, blkT = 0.0;
    if (s_blkN >= 2u)
    {
        blkMean = s_blkSum / static_cast<double>(s_blkN);
        const double var = (s_blkSq - static_cast<double>(s_blkN) * blkMean * blkMean) /
                           static_cast<double>(s_blkN - 1u);
        blkSe = var > 0.0 ? std::sqrt(var / static_cast<double>(s_blkN)) : 0.0;
        blkT = blkSe > 0.0 ? blkMean / blkSe : 0.0;
    }
    std::fprintf(stderr,
                 "[G419:ab] lever=%s ballast=%d hold=%llu frames=%llu drop=%llu trans=%llu "
                 "cand(n=%llu ms=%.3f se=%.3f) ctrl(n=%llu ms=%.3f se=%.3f) "
                 "delta=%.3f (%.2f%%) seDelta=%.3f "
                 "blocked(w=%llu mean=%.3f se=%.3f t=%.2f fav=%llu)\n",
                 g419AbLeverName(g419AbLever()),
                 g419AbBallast(),
                 static_cast<unsigned long long>(g438ArmHoldFrames()),
                 static_cast<unsigned long long>(idx),
                 static_cast<unsigned long long>(s_dropped),
                 static_cast<unsigned long long>(s_transition),
                 static_cast<unsigned long long>(s_n[0]), c, sc,
                 static_cast<unsigned long long>(s_n[1]), k, sk,
                 c - k, k != 0.0 ? 100.0 * (c - k) / k : 0.0,
                 std::sqrt(sc * sc + sk * sk),
                 static_cast<unsigned long long>(s_blkN), blkMean, blkSe, blkT,
                 static_cast<unsigned long long>(s_blkFav));

    // G447 host check: turn the paired statistic into a verdict a reader who has never seen this
    // instrument can act on, and flag the two ways the run can measure NOTHING.
    if (g447HostCheckOn() && g419AbLever() == static_cast<int>(kG447LeverSpinBoth))
    {
        int logical = 0, physical = 0;
        g447HostShape(&logical, &physical);
        // G443: DC2 is a locked-30 title. A run whose work drops below 33.33 ms/f reads ~33.3 with
        // se ~0.01 on BOTH arms — that is the cap, not a result.
        const bool capped = (c > 33.0 && c < 33.8 && k > 33.0 && k < 33.8 && sc < 0.03 && sk < 0.03);
        const char *verdict;
        if (capped)
            verdict = "INVALID-FRAME-CAP: rerun with DC2_PATCH_60FPS=1 on BOTH arms (G443)";
        else if (s_blkN < 3u)
            verdict = "INCONCLUSIVE: too few windows, run longer (want w>=4)";
        else if (blkT <= -3.0 && s_blkFav * 2u >= s_blkN * 2u - 2u)
            verdict = "KEEP DEFAULT: the spins pay on this host";
        else if (blkT >= 3.0)
            verdict = "ROLL BACK: set DC2_G447_NO_SPIN=1 and DC2_G447_NO_WSPIN=1 on this host";
        else
            verdict = "NEUTRAL: no effect either way on this host; default is harmless";
        std::fprintf(stderr,
                     "[G447:host] cpus=%d logical / %d physical | runtime uses 4 busy threads + %d "
                     "spinners | spinMs/f=%.3f se=%.3f t=%.2f windows=%llu favouring=%llu | "
                     "reference host (6 physical/12 logical): -1.14 ms/f t=-7.19 4/4 | %s\n",
                     logical, physical, 2, blkMean, blkSe, blkT,
                     static_cast<unsigned long long>(s_blkN),
                     static_cast<unsigned long long>(s_blkFav), verdict);
    }
}

// ---------------------------------------------------------------------------------------------
// Exact CT32 upload fast pack.

// G419 PROMOTED to default-ON (2026-07-30). Exact by construction (the table IS addrPSMCT32
// restricted to one page, paired loads rest on the one-word columnTable32 adjacency) and by oracle
// (643,149,824 pixels, bad=0). Isolated cost 1.032 -> 0.767 ms/frame against G298's row-gated lanes
// and 0.730 -> 0.371 against the pixel gate; within-process randomized A/B over 6,000 MAP-0 frames
// measured -0.426 ms/frame (blocked -0.394, se 0.169, t = -2.33, 7/10 windows favouring).
// Kill with DC2_G419_NO_FAST_PACK=1 or DC2_G419_FAST_PACK=0.
static bool g419FastPackOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_FAST_PACK") &&
                           !envFlagDisabled("DC2_G419_FAST_PACK");
    return on;
}

// G419 pixel-gated row-pool fan-out for the upload pack. PROMOTED to default-ON (2026-07-30):
// isolated 1.032 -> 0.730 ms/frame against the legacy pack and 0.767 -> 0.371 against the fast pack,
// and within-process randomized A/B over 5,400 MAP-0 frames measured -0.320 ms/frame
// (blocked -0.303, se 0.137, t = -2.22, 6/9 windows favouring).
// Kill with DC2_G419_NO_PACK_LANE_GATE=1 or DC2_G419_PACK_LANE_GATE=0.
//
// G298 gates the pack's row-pool fan-out on `packRows >= 8`. Rows cannot express the work: DC2's
// small upload targets pack 128 rows of a 128-pixel target (16k pixels) or 64 rows of a 64-pixel one
// (4k pixels), which clear a row gate while carrying 1/13th to 1/53rd of a 416x512 pack. Measured on
// the current binary (DC2_G419_PROFILE, exact fast pack, same call counts):
//
//     target      geometry    8 lanes   1 lane
//     0x139/w8    448x512     0.102     0.152   <- fan-out pays
//     0x0/w8      416x512     0.102     0.134   <- pays
//     0x68/w8     416x512     0.096     0.137   <- pays
//     0x139/w2    128x128     0.083     0.012   <- fan-out costs 7x the whole loop
//     0x13c/w2    128x128     0.076     0.011
//     0x146/w2    128x128     0.077     0.013
//     0x143/w1     64x64      0.079     0.004
//     0x155/w1     64x64      0.076     0.004
//
// So GSRowPool dispatch is a fixed ~0.085 ms on this route (8 lane wake-ups + join). Break-even for
// an 8-lane split is therefore W > D*8/7, i.e. about 0.097 ms of serial work, which at the measured
// 0.63 ns/pixel is about 154k pixels. The DC2 population is bimodal (213k-229k vs 4k-16k), so any
// threshold in the gap is safe; 65,536 stays well clear of both clusters.
//
// EXACT BY CONSTRUCTION: lane count never changes results. G298's contract is that disjoint guest
// row ranges own disjoint VRAM/staging words, so one lane is simply the serial reference. Verified
// by DC2_G298_VERIFY_FBPACK / DC2_G419_VERIFY, which re-derive every packed pixel serially.
static bool g419PackLaneGateOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_PACK_LANE_GATE") &&
                           !envFlagDisabled("DC2_G419_PACK_LANE_GATE");
    return on;
}

static uint64_t g419PackLaneMinPixels()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_LANE_MIN_PX");
        if (value == nullptr)
            return 65536;
        return std::max(0, std::atoi(value));
    }();
    return n;
}

// `requested` is G298's existing lane count (already row-gated). Returns the lane count to dispatch.
static int g419PackLanes(int packRows, int fbW, int requested)
{
    if (requested <= 1 || packRows <= 0 || fbW <= 0)
        return requested;
    if (!g419AbArmFor(kG419LeverLanes, g419PackLaneGateOn()))
        return requested;
    const uint64_t pixels = static_cast<uint64_t>(packRows) * static_cast<uint64_t>(fbW);
    return pixels >= g419PackLaneMinPixels() ? requested : 1;
}

// Behaviour-pure isolated timer for the pack loop, keyed by target/width. Unlike DC2_G417_PROFILE
// this arms NOTHING in the GL backend, so it does not insert a finish and does not perturb
// scheduling - it is safe to read alongside a normal run.
static bool g419ProfileOn()
{
    static const bool on = envFlagEnabled("DC2_G419_PROFILE");
    return on;
}

struct G419Ct32Page
{
    uint32_t block = 0u;
    uint32_t width = 0u;
    bool valid = false;
    uint32_t off[32][64]{};
};

// Uploads visit several (block,width) pairs per frame (fbp=0x139 at fbw=8 AND fbw=2, plus 0x0,
// 0x68 and singletons), so a single-entry cache would rebuild 2048 addresses per upload. Twelve
// slots cover the observed DC2 population with round-robin replacement as the fail-safe.
static G419Ct32Page s_g419Ct32[12];
static uint32_t s_g419Ct32Next = 0u;

static const G419Ct32Page &g419Ct32Table(uint32_t block, uint32_t width)
{
    const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
    for (G419Ct32Page &e : s_g419Ct32)
        if (e.valid && e.block == block && e.width == pagesPerRow)
            return e;
    G419Ct32Page &e = s_g419Ct32[s_g419Ct32Next % 12u];
    ++s_g419Ct32Next;
    const uint32_t base = (block >> 5u) << 13u;
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 64u; ++x)
            e.off[y][x] = GSPSMCT32::addrPSMCT32(block, pagesPerRow, x, y) - base;
    e.block = block;
    e.width = pagesPerRow;
    e.valid = true;
    return e;
}

// Pack guest rows [laneLo,laneHi] of a CT32 target out of VRAM into the bottom-first staging buffer.
// `legacy` runs the original serial per-pixel path verbatim as the control arm.
static void g419PackFbRows(uint32_t *dst, size_t dstCount,
                           const uint8_t *vram, uint32_t vramSize,
                           uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                           int laneLo, int laneHi, bool legacy)
{
    if (dst == nullptr || vram == nullptr || laneHi < laneLo || fbW <= 0)
        return;
    if (legacy || (fbW & 1) != 0)
    {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
            if (dstRow + static_cast<size_t>(fbW) > dstCount)
                continue;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                           static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y));
                uint32_t px = 0u;
                if (off + 4u <= vramSize)
                    std::memcpy(&px, vram + off, 4);
                dst[dstRow + x] = px;
            }
        }
        return;
    }
    const G419Ct32Page &tbl = g419Ct32Table(fbBlock, fbw);
    const uint32_t pagesPerRow = (fbw != 0u) ? fbw : 1u;
    const uint32_t basePage = fbBlock >> 5u;
    for (int y = laneLo; y <= laneHi; ++y)
    {
        const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
        if (dstRow + static_cast<size_t>(fbW) > dstCount)
            continue;
        const uint32_t *rowOff = tbl.off[static_cast<uint32_t>(y) & 31u];
        const uint32_t yBase =
            (basePage + (static_cast<uint32_t>(y) >> 5u) * pagesPerRow) << 13u;
        uint32_t *dstPx = dst + dstRow;
        for (int x = 0; x < fbW; x += 2)
        {
            const uint32_t off = yBase +
                                 (static_cast<uint32_t>(x >> 6) << 13u) +
                                 rowOff[static_cast<uint32_t>(x) & 63u];
            // Pixel x sits at off and pixel x+1 at off+4 (one-word columnTable32 adjacency), so the
            // original loop's per-pixel bound test maps onto exactly these three cases.
            if (off + 8u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 8);
            }
            else if (off + 4u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 4);
                dstPx[x + 1] = 0u; // the original wrote 0 when off+4 (here off+8) exceeded VRAM
            }
            else
            {
                dstPx[x] = 0u;
                dstPx[x + 1] = 0u;
            }
        }
    }
}

// DC2_G419_PROFILE aggregate for the isolated pack cost, keyed by target and staging geometry.
struct G419PackStat
{
    uint64_t n = 0, rows = 0, ns = 0;
};
static std::map<uint64_t, G419PackStat> s_g419Packs; // flush-owner thread only
static uint64_t s_g419PackTotal = 0u;

static void g419NotePack(uint32_t fbp, uint32_t fbw, int rows, uint64_t ns)
{
    const uint64_t key = (static_cast<uint64_t>(fbp) << 8) | (fbw & 0xffu);
    G419PackStat &s = s_g419Packs[key];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.ns += ns;
    if ((++s_g419PackTotal % 512u) != 0u)
        return;
    // `perF` is calls per FRAME and `msF` is this target's whole contribution to a frame. msF is the
    // only number a payoff prediction may use; per-call cpu is a mechanism figure.
    const double frames = s_g419FrameTicks != 0u ? static_cast<double>(s_g419FrameTicks) : 1.0;
    double sumMsF = 0.0;
    std::fprintf(stderr, "[G419:pack] total=%llu frames=%llu fast=%d",
                 static_cast<unsigned long long>(s_g419PackTotal),
                 static_cast<unsigned long long>(s_g419FrameTicks),
                 g419FastPackOn() ? 1 : 0);
    for (const auto &kv : s_g419Packs)
    {
        const G419PackStat &e = kv.second;
        if (e.n == 0u)
            continue;
        const double msF = static_cast<double>(e.ns) / 1.0e6 / frames;
        sumMsF += msF;
        std::fprintf(stderr, " fbp=0x%x/w%u(n=%llu perF=%.2f rows=%.1f cpu=%.3f msF=%.3f)",
                     static_cast<unsigned>(kv.first >> 8),
                     static_cast<unsigned>(kv.first & 0xffu),
                     static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.n) / frames,
                     static_cast<double>(e.rows) / e.n,
                     static_cast<double>(e.ns) / 1.0e6 / e.n,
                     msF);
    }
    std::fprintf(stderr, " TOTmsF=%.3f\n", sumMsF);
}
