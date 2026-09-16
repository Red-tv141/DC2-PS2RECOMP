// ==============================================================================================
// G713 — THE GS EXECUTION PIPELINE: a PARSE stage and an EXEC stage on two cores
// ==============================================================================================
//
// THE MEASUREMENT THAT DEFINES THIS PHASE. `[G332:gsw]` + `[G147:gif]` + `[G146:perf]` on the
// shipped binary, the four hardest routes' heaviest windows (tools/g713_split.py):
//
//   route      GS own | parse   l2l  draw | image (rend  rdbk) | rest
//   s05         32.55 |  3.75  4.03  4.72 | 15.26 (3.06  5.94) | 4.79
//   s03         36.83 |  4.56  3.21  5.51 | 17.82 (3.80  4.16) | 5.71
//   fight       36.02 |  4.12  2.34  6.95 | 17.05 (4.15  4.71) | 5.56
//   dungeon1    27.04 |  3.27  1.82  3.70 | 14.47 (3.81  4.16) | 3.76
//
//   parse = [G147:gif] parserOther MINUS [G146:perf] GSlocal — `performLocalToLocalTransfer` is
//           called from `writeRegister(TRXDIR)` and is therefore charged inside parserOther.
//   draw  = [G147:gif] draw   (the drawPrimitive capture)
//   image = [G146:perf] GSimage (the host->local writer AND the whole g260 drain nested in it)
//
// ⭐⭐⭐ `parse + draw` is **7.0-11.1 ms/f, 26-31% of GS own**, and it is PURE CPU: the GIF tag
// walk, the register decode and the 256-byte entry capture touch no guest VRAM and no GL. That was
// established by reading the code, not inferred from timers:
//
//   * `GS::writeRegister` (gpu_gif_and_registers.inc:584..1008) escapes to memory or the GPU on
//     exactly THREE lines of its whole 16-way switch — `performLocalToLocalTransfer`,
//     `performLocalToHostToBuffer` and `processImageData`, all under `TRXDIR`/`HWREG`. Every other
//     case writes GS register state and nothing else.
//   * `GS::writeRegisterPacked` / `vertexKick` write `m_vtxQueue` and call `drawPrimitive`.
//   * `drawPrimitive`'s DEFERRABLE path — the 99%+ population — evaluates a predicate over
//     registers and three address registries and appends a self-contained `G144Entry`.
//     `e.decoded` is `g149BuildDecoded(...)`, which returns null unless `DC2_G149_TEXCACHE` is set,
//     so in the shipped build the capture does not even read texture memory.
//   * `g178TryFlushGpu`, 7,830 lines, dereferences `gs` on exactly FOUR lines. The flush is a
//     function of (entries, VRAM, residency, GL) — never of live GIF parse state.
//
// So the GS worker is one thread doing two things that do not share state: it PARSES a byte stream
// into immutable batches, and it EXECUTES those batches against VRAM and the GPU. This file makes
// them two threads.
//
//   parse (the existing GS worker)                exec (this file's thread; owns GL and VRAM)
//   ------------------------------                -------------------------------------------
//   GIF tag walk, register decode                 g_g260Graph push + g260ExecuteAll (the flush)
//   vertexKick, drawPrimitive capture             GS::processImageData + its upload edge
//   g260CloseOpenBatch  ------- post kBatch --->  performLocalToLocal / performLocalToHost
//   BITBLTBUF/TRXPOS/TRXREG/TRXDIR  post kXfer->  the non-deferrable inline rasterisation (kCall)
//   IMAGE bytes         ------- post kImage --->  the frame-boundary closure (kCall)
//
// ⛔⛔ WHY THIS IS NOT G701/G684/G690/G704 (async submit, refuted four times). Those kept ONE
// thread's work and removed its WAIT, so the core it used to yield went to nobody and the GL worker
// starved; G704 closed the direction on that mechanism. Here both threads carry measured, disjoint
// work — 7-11 ms/f and 16-26 ms/f — so neither is idling on the other's behalf, and the GL context
// never changes hands mid-run: exec adopts it once. G705's fused backend is untouched; it simply
// fuses onto the exec thread instead of the parse thread, because `g178_backend_ready()`'s single
// call site (the top of `g178TryFlushGpu`) is now reached from exec.
//
// ⛔ AND IT IS NOT G712's PREP-AHEAD. That moved ONE flush stage (`class`, 0.405 ms/f) and G712
// §4.2 bounded the whole capture-time-preparation axis at ~1.0 ms/f, because every other stage is
// GL-ordered or VRAM-timed. This moves the other side of the cut: not a stage of the flush, but
// everything that is not the flush.
//
// ORDERING IS THE ONLY CORRECTNESS OBLIGATION, AND IT IS EXPLICIT. Every exec-side operation is
// posted in guest order into one SPSC ring and consumed in that order by one thread, so a VRAM
// operation can never be reordered against a CLOSED batch — the batch is already in the ring ahead
// of it. The one thing that CAN be reordered is the OPEN batch, whose entries were captured before
// a VRAM node but would execute after it. `closeForUpload` / `closeForLocalCopy` apply the g260
// range test to exactly that batch; a true answer closes and posts it first. A false answer means
// the two are provably independent and their order is unobservable — the same theorem
// `g260VramEventNeedsExecute` already relies on to SKIP a drain today.
//
// ⭐ G725: PROMOTED DEFAULT ON. The order-balanced A B B A gate on `dungeon1` (tag g721gate2k1,
// ONE binary on both arms, arm proven present/absent) read control 22.290 / 21.531 against
// candidate 18.363 / 18.328 over the SAME 5280-frame common script window — pooled **-3.565 ms/f
// (-16.3%)**, every band strongly negative (-2.704 / -3.847 / -4.125), workload identity
// `sf/renderedFrame` 2.214 on all four arms, control drift 0.759 i.e. 4.7x smaller than the
// effect. Graphics: MAP-0 (the tile-bin/MTGS/pipelining row of the route matrix), 80 script-keyed
// frames, candidate below the independent control-repeat noise against BOTH controls, 30 frames
// byte-identical. The lever is therefore ON unless explicitly rolled back.
//
// ARMS
//   DC2_G713_NO_PIPE=1 / DC2_G721_NO_PIPE=1   ⛔ ROLLBACK (restores the serial GS front end)
//   DC2_G713_PIPE=1        the old bring-up arm; now redundant but still accepted, so every
//                          harness script and ledger command line from G713..G724 keeps working
//   DC2_G713_SERIAL=1      ⭐ ORACLE MODE: the ring is used but drained INLINE at every post, so no
//                          work crosses a thread. A SERIAL run must be pixel-identical to the
//                          shipped path; it isolates "is the decomposition into exec operations
//                          correct" from "is the threading correct". Always gate SERIAL first.
//   DC2_G713_PIPE_STAT=1   `[G713:pipe]` census every 240 frame boundaries
//   DC2_G713_RING_MB=<n>   IMAGE payload ring, default 24 MiB (routes peak at 2.5 MB/f)
// ==============================================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <emmintrin.h> // G726: _mm_pause for the pre-park spin in g713Sync
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ⭐ G735: `GetThreadTimes` for the executor's REAL CPU column (see g713_exec_cpu_ns). This TU had
// no Windows header; the include is confined to the same `_WIN32` guard the accessor uses, and
// NOMINMAX / WIN32_LEAN_AND_MEAN keep it from colliding with `std::min`/`std::max` used above.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ps2_g713_pipeline_api.inc"

// ⭐ G737: two diagnostics that must survive a RELEASE build are hosted here, for the reason this
// TU already hosts `g735ThreadCpuNs` — it is COLD, and it already carries <windows.h>. Their call
// sites are in `ps2_gif_arbiter.cpp` (the PC sampler) and `ps2_gs_rasterizer.cpp` (the FBO-bind
// refusal), both of which hold hot loops that G710 measured at +0.362 ms/f for compile-time-DEAD
// text alone. Neither file gains a classifier; each gains a declaration and a call.
#include "ps2_g737_route_census_api.inc"
#include "ps2_g737_route_census.inc"
#include "ps2_g737_pcsample_pairs.inc"

// ⭐⭐⭐ G738: the GL command-stream census reporter, hosted here for the same reason. Its counters
// live in the raster TU's anonymous namespace and are read through the global-scope
// `g738_glcall_snapshot` bridge; only the formatting is here.
#include "ps2_g738_glcall_api.inc"
// The three clocks `[G738:clocks]` compares. Global scope, defined elsewhere (present tick in
// ps2_runtime.cpp, the other two in ps2_memory.cpp) — this TU has no anonymous namespace above
// this point, so a plain extern is correct here.
extern std::atomic<uint64_t> g_dc2PresentTick;
extern std::atomic<uint32_t> g_dc2ScriptFrame;
#include "ps2_g738_glcall.inc"

namespace
{

bool g713EnvOn(const char *name)
{
    const char *v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

uint32_t g713EnvU32(const char *name, uint32_t dflt)
{
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return dflt;
    const uint32_t r = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    return r ? r : dflt;
}

// ⭐ G734: is the acceptance BOARD armed? The board's `[G734:gsx]` column is printed beside
// `[G182:ee]` and gated on the same flag, so the executor's busy/idle accumulation is gated on it
// too — one predicted branch per drain cycle in a shipping run, instead of clock reads.
bool g734BoardOn()
{
    static const bool on = g713EnvOn("DC2_G182_EE_STAT");
    return on;
}

enum
{
    kNodeXfer = 0,  // TRXDIR edge: apply the register snapshot, run local->local / local->host
    kNodeImage = 1, // host->local IMAGE payload (register snapshot + bytes)
    kNodeBatch = 2, // one closed, immutable G260Batch; ownership transfers to exec
    kNodeCall = 3,  // fn(arg) on exec, in ring order (the poster then blocks in g713Sync)
    kNodeKindN = 4
};

constexpr uint32_t kNodeCap = 8192u; // power of two

struct G713Node
{
    uint32_t kind;
    uint32_t payLen;
    void *ptr;              // G260Batch* | call argument
    void (*call)(void *);   // kNodeCall only
    uint64_t payOff;        // byte-ring offset of an IMAGE payload
    G713XferSnap snap;
};

struct G713Ring
{
    G713Node node[kNodeCap];
    std::vector<uint8_t> pay;
    uint64_t payCap = 0;

    alignas(64) uint64_t head = 0; // producer-owned; never share its line with consumer cursors
    uint64_t payHead = 0; // producer-owned: next payload byte to write
    alignas(64) uint64_t tailC = 0; // consumer-owned
    uint64_t payTailC = 0;

    alignas(64) std::atomic<uint64_t> headPub{0};
    alignas(64) std::atomic<uint64_t> tailPub{0};
    std::atomic<uint64_t> payTailPub{0};

    // G721: SPSC events. The old CV predicate was published without its mutex,
    // allowing a notification between predicate check and parking to be lost.
    // Clearing with acquire-release exchange, then rechecking the queue, makes
    // each event safe even when a producer raced the transition into sleep.
    struct Event
    {
        std::atomic<bool> pending{false};
        void notify()
        {
            pending.store(true, std::memory_order_release);
            pending.notify_one();
        }
        template<class Predicate> void wait(Predicate ready)
        {
            pending.exchange(false, std::memory_order_acq_rel);
            if (!ready()) pending.wait(false, std::memory_order_acquire);
        }
    } work, space;
    std::atomic<bool> stop{false};
    std::thread thread;

    G713RasterHooks raster{};
    G713GsHooks gs{};
    void (*threadInit)() = nullptr;

    std::atomic<uint64_t> posted{0}, byKind[kNodeKindN]{}, blockedPost{0}, blockedPay{0},
        syncWaits{0}, syncNs{0}, payBytes{0}, maxDepth{0}, execNs{0};
    // ⭐ G734: the EXECUTOR's own IDLE. `execNs` measures the exec thread WORKING; nothing measured
    // it WAITING, so the difference between execMs/f and the frame was unowned and the thread's
    // occupancy could not be stated. Without this the three-thread picture is two-thirds of a
    // picture: EE's park (m_cvFrameSlot) and the parse thread's park (syncNs) were both already
    // instrumented, and they are both DOWNSTREAM of this one. STAT-only, one clock pair per drain
    // cycle (not per node), so it cannot perturb a timing arm it is not armed in.
    std::atomic<uint64_t> idleNs{0}, idleWaits{0};
    // ⭐⭐⭐ G734: MONOTONIC executor totals for the acceptance BOARD, never reset and never
    // stat-gated (the two above are exchanged by `[G713:pipe]`, so the board cannot share them).
    //
    // ⛔ WHY THIS EXISTS. The board's GS column is `gsOwn = gsWorker - gsStall`, and `gsWorker` is
    // `g303_gs_worker_busy_ns()` = `g_g151WorkerBusyNs`, which the arbiter's own source documents as
    // "window-ONLY, so it under-reports the pole". Since G726 split the GS worker into a parse
    // thread and this executor thread, `gsOwn` has scored the GS pipeline by the parse thread's
    // window branch alone - omitting the frame-boundary branch AND this entire thread. On
    // `dungeon1` that is 7.8-8.2 ms/f reported against ~17 ms/f actually executing here, so the
    // classifier `max(gsOwn, vu1Busy, eeWork)` labelled the route EE-limited when the executor is
    // the busiest thread in the process. Cost: two clock pairs per DRAIN CYCLE (~12/frame), not per
    // node - about a microsecond a frame.
    std::atomic<uint64_t> execTotalNs{0}, idleTotalNs{0};
    // G724: where does the EXECUTOR's time actually go? `execNs` says 16.1 ms/f while the flush's
    // own stage census says 12.5, and the ~2.4 ms/f difference had no owner. The upload prologue
    // census settled that it is NOT the image writer (`[G313:seg] writer` is 11% of a span whose
    // other 89% is the barrier that CONTAINS the flush). Time each node KIND instead, so batch
    // execution, image application and transfers are separated by construction. STAT-only.
    std::atomic<uint64_t> nsByKind[kNodeKindN]{};
    // ⭐ G726: blocking round trips since the last frame boundary. Unlike `byKind[kNodeCall]` this
    // is NOT stat-gated - the adaptive fallback has to work in a shipping build, and it is the one
    // counter that decides whether threading pays on this route. One relaxed increment per blocking
    // call, i.e. per event that already costs a full cross-thread round trip.
    std::atomic<uint32_t> callsFrame{0};
};

G713Ring &ring()
{
    static G713Ring r;
    return r;
}

bool s_serial = false;
// G726 adaptive fallback: consecutive frames whose blocking-call rate exceeded the threshold.
// Parse-thread only (read and written at the frame boundary), so a plain int is correct.
uint32_t s_adaptHot = 0u;
bool s_armed = false;
thread_local bool t_onExec = false;

void g713RunNode(G713Ring &r, G713Node &n)
{
    switch (n.kind)
    {
    case kNodeXfer:
        if (r.gs.xfer)
            r.gs.xfer(&n.snap);
        break;
    case kNodeImage:
        if (r.gs.image && n.payLen)
            r.gs.image(&n.snap, r.pay.data() + (n.payOff % r.payCap), n.payLen);
        break;
    case kNodeBatch:
        if (r.raster.execBatch)
            r.raster.execBatch(n.ptr);
        break;
    case kNodeCall:
        if (n.call)
            n.call(n.ptr);
        break;
    default:
        break;
    }
}

// Consume [tailC, head). Shared by the exec thread and by SERIAL mode's inline drain, so both
// modes run byte-identical node bodies in byte-identical order.
void g713Consume(G713Ring &r, uint64_t head, bool threaded)
{
    const bool stat = g713PipeStatOn();
    // G734: `execTotalNs` feeds the acceptance BOARD, which runs without `DC2_G713_PIPE_STAT` —
    // so it cannot share `execNs` (that one is `exchange`d by `[G713:pipe]`). Gated on the board's
    // own flag rather than left unconditional: a shipping run then pays ONE perfectly-predicted
    // branch per drain cycle (~12/frame) instead of two `QueryPerformanceCounter` reads, which
    // keeps FINAL §7's "dead code is not free in a hot TU" exposure at its minimum.
    //
    // ⛔⛔⛔ G743 — THIS GATE MADE `execMs/f` A STRUCTURAL ZERO UNDER `DC2_G713_PIPE_STAT` ALONE.
    // `[G713:pipe]` prints `execMs/f` and `[G734:exec]` prints `idleMs/f` whenever `stat` is set,
    // but BOTH accumulators were gated on `board` (= `DC2_G182_EE_STAT`) only. A run armed with
    // `DC2_G713_PIPE_STAT=1` and nothing else therefore printed
    //     [G713:pipe] ... execMs/f=0.000
    //     [G734:exec] idleMs/f=0.000 idleWaits/f=0.00
    // on every line while `[G713:kind]` — documented two lines below as summing TO `execMs/f` —
    // reported 17.6 ms/f on the same thread. A subset larger than its parent, printed as a
    // measurement, with a comment beside it instructing the reader to conclude "SATURATED".
    // Fixed by gating the accumulation on `board || stat`, which is what each printer requires.
    const bool board = g734BoardOn();
    const bool timed = board || stat;
    const auto begin = timed ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    // ⭐⭐ G743 — THE POLE THREAD'S WORK DERIVATIVE. The project ships a fixed busy-spin knob for
    // the VU1 worker (`DC2_G303_VU1_SLOW_US`, per kick), the GS parse worker
    // (`DC2_G431_GS_SLOW_US`, per window) and the EE thread (`DC2_G503_EE_SLOW_US`) — and none for
    // the G713 executor, which is the thread every board since G734 has called the pole. Without
    // it, "would removing N ms of executor work remove N ms of frame?" can only be argued from
    // occupancy columns, which is the exact mistake G735 Rule 12 was written about.
    //
    // Injected per NODE (`posted/f` is ~196 on `dungeon1`, so `=10` is ~1.96 ms/f), inside the
    // interval `execNs`/`[G713:kind]` measure, so the injected cost shows up in this thread's own
    // columns as well as in the frame. Default 0: one perfectly-predicted branch per node.
    static const long long s_g743ExecSlowUs = [] () -> long long {
        const char *v = std::getenv("DC2_G743_EXEC_SLOW_US");
        return v ? std::atoll(v) : 0LL;
    }();
    while (r.tailC != head)
    {
        if (s_g743ExecSlowUs > 0)
        {
            const auto spinUntil = std::chrono::steady_clock::now() +
                                   std::chrono::microseconds(s_g743ExecSlowUs);
            while (std::chrono::steady_clock::now() < spinUntil) { /* executor only */ }
        }
        G713Node &n = r.node[r.tailC & (kNodeCap - 1u)];
        if (stat)
        {
            const auto k0 = std::chrono::steady_clock::now();
            g713RunNode(r, n);
            if (n.kind < kNodeKindN)
                r.nsByKind[n.kind].fetch_add(
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - k0).count()),
                    std::memory_order_relaxed);
        }
        else
            g713RunNode(r, n);
        if (n.kind == kNodeImage)
            r.payTailC = n.payOff + n.payLen;
        ++r.tailC;
        if (threaded && ((r.tailC & 63u) == 0u || r.tailC == head))
        {
            r.tailPub.store(r.tailC, std::memory_order_release);
            r.payTailPub.store(r.payTailC, std::memory_order_release);
            r.space.notify(); // publish a group, not one wakeup per IMAGE fragment
        }
        else if (!threaded)
        {
            r.tailPub.store(r.tailC, std::memory_order_relaxed);
            r.payTailPub.store(r.payTailC, std::memory_order_relaxed);
        }
    }
    if (timed)
    {
        const uint64_t cycleNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        if (board)
            r.execTotalNs.fetch_add(cycleNs, std::memory_order_relaxed); // G734: board, never reset
        if (stat)
            r.execNs.fetch_add(cycleNs, std::memory_order_relaxed);
    }
}

void g713ExecLoop()
{
    G713Ring &r = ring();
    t_onExec = true;
    if (r.threadInit)
        r.threadInit();
    // ⭐ G735: duplicate a handle to THIS thread so the board can read its real CPU with
    // GetThreadTimes from the EE thread's frame boundary (see g713_exec_cpu_ns). One call, once.
    g735ExecCaptureThreadHandle();
    for (;;)
    {
        uint64_t head = r.headPub.load(std::memory_order_acquire);
        if (r.tailC == head)
        {
            // G734: price the executor's IDLE. `idleTotalNs` is what lets the board state the
            // executor's OCCUPANCY — busy time alone cannot distinguish a saturated thread from a
            // slow one, and that distinction is the whole verdict on the EE park. Same board flag
            // as the busy side, one predicted branch per drain cycle.
            // ⛔ G743: `idleNs`/`idleWaits` are printed by `[G734:exec]` under `DC2_G713_PIPE_STAT`,
            // so they must be ACCUMULATED under it too — see the long note in `g713Consume`.
            const bool g734Board = g734BoardOn();
            const bool g743Stat = g713PipeStatOn();
            const bool g743Timed = g734Board || g743Stat;
            const auto g734T0 = g743Timed ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
            r.work.wait([&] {
                return r.stop.load(std::memory_order_relaxed) ||
                       r.headPub.load(std::memory_order_acquire) != r.tailC;
            });
            if (g743Timed)
            {
                const uint64_t g734IdleNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g734T0).count());
                if (g734Board)
                    r.idleTotalNs.fetch_add(g734IdleNs, std::memory_order_relaxed);
                if (g743Stat)
                {
                    r.idleWaits.fetch_add(1u, std::memory_order_relaxed);
                    r.idleNs.fetch_add(g734IdleNs, std::memory_order_relaxed);
                }
            }
            if (r.stop.load(std::memory_order_relaxed) &&
                r.headPub.load(std::memory_order_acquire) == r.tailC)
                return;
            head = r.headPub.load(std::memory_order_acquire);
        }
        g713Consume(r, head, true);
    }
}

// ⛔⛔⛔ SERIAL MODE IS NOT "THE SAME CODE WITHOUT A THREAD" UNLESS IT ALSO OWNS `t_onExec`.
//
// The first SERIAL build died with STATUS_STACK_OVERFLOW (0xc00000fd) after ~1,300 frames, and the
// mechanism is the whole reason this guard exists. Every producer site is written
// `if (g713PipeArmed() && !g713OnExecThread()) post(...); else <shipped path>`. On the threaded
// arm the exec thread sets `t_onExec`, so a node body that reaches a producer site takes the
// SHIPPED path — which is correct, because it IS the exec side. In SERIAL there was no exec
// thread, so `t_onExec` stayed false and a node body reaching a producer site POSTED AGAIN and
// drained again: node → body → post → drain → node → … until the stack ran out.
//
// So the inline drain claims the exec identity for its duration, and refuses to re-enter itself: a
// post made from inside a node body is appended and picked up by the outermost loop, which is
// exactly the order the threaded arm produces. That makes SERIAL a real oracle instead of a
// different program — the node bodies and their order become byte-identical to the threaded arm's.
bool s_drainingInline = false;

void g713DrainInline(G713Ring &r)
{
    if (s_drainingInline)
        return; // a nested post: the outermost loop below will consume it, in order
    s_drainingInline = true;
    const bool wasExec = t_onExec;
    t_onExec = true;
    // Re-read `headPub` each pass: a node body may append more nodes, and they must run here.
    while (r.tailC != r.headPub.load(std::memory_order_relaxed))
        g713Consume(r, r.headPub.load(std::memory_order_relaxed), false);
    t_onExec = wasExec;
    s_drainingInline = false;
}

// Reserve one descriptor slot. Blocks only if the consumer is a whole ring behind.
G713Node &g713Acquire(G713Ring &r)
{
    if (r.head - r.tailPub.load(std::memory_order_acquire) >= kNodeCap - 2u)
    {
        r.blockedPost.fetch_add(1u, std::memory_order_relaxed);
        while (!r.stop.load(std::memory_order_relaxed) &&
               r.head - r.tailPub.load(std::memory_order_acquire) >= kNodeCap - 2u)
            r.space.wait([&] {
                return r.stop.load(std::memory_order_relaxed) ||
                       r.head - r.tailPub.load(std::memory_order_acquire) < kNodeCap - 2u;
            });
    }
    return r.node[r.head & (kNodeCap - 1u)];
}

void g713Publish(G713Ring &r, uint32_t kind)
{
    ++r.head;
    r.headPub.store(r.head, std::memory_order_release);
    if (g713PipeStatOn())
    {
        r.posted.fetch_add(1u, std::memory_order_relaxed);
        r.byKind[kind].fetch_add(1u, std::memory_order_relaxed);
    }
    if (s_serial)
    {
        g713DrainInline(r);
        return;
    }
    if (g713PipeStatOn())
    {
        const uint64_t depth = r.head - r.tailPub.load(std::memory_order_relaxed);
        if (depth > r.maxDepth.load(std::memory_order_relaxed))
            r.maxDepth.store(depth, std::memory_order_relaxed);
    }
    r.work.notify();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Registration + arming
// ---------------------------------------------------------------------------------------------

void g713SetRasterHooks(const G713RasterHooks &h) { ring().raster = h; }
void g713SetGsHooks(const G713GsHooks &h) { ring().gs = h; }
void g713SetThreadInit(void (*threadInit)()) { ring().threadInit = threadInit; }

bool g713PipeEnabled()
{
    // G725: DEFAULT ON (see the promotion note at the top of this file). The two positive arms are
    // kept only so existing command lines still mean what they meant; they no longer decide
    // anything. Two rollback spellings, because G721 renamed the master arm mid-phase and both
    // names appear in the harness clear list and in every ledger command line.
    static const bool on = !g713EnvOn("DC2_G713_NO_PIPE") && !g713EnvOn("DC2_G721_NO_PIPE");
    return on;
}

bool g713PipeSerialMode()
{
    static const bool on = g713EnvOn("DC2_G713_SERIAL");
    return on;
}

bool g713PipeStatOn()
{
    static const bool on = g713EnvOn("DC2_G713_PIPE_STAT");
    return on;
}

// ⭐ BISECT LEVERS. The mechanism has two independent halves and they fail independently, so each
// gets its own kill. `DC2_G713_NO_IMAGE=1` leaves the host→local IMAGE stream and the TRXDIR seam
// on the parse thread (the shipped path) while geometry batches still cross to exec;
// `DC2_G713_NO_BATCH=1` does the opposite. ⚠️ Each is DIAGNOSTIC ONLY — with one half on each
// thread there is no ordering guarantee between them, so neither arm is a correctness candidate.
// They exist to answer "which half derails", which reading the diff could not.
bool g713PipeImageOn()
{
    static const bool on = !g713EnvOn("DC2_G713_NO_IMAGE");
    return on;
}

bool g713PipeBatchOn()
{
    static const bool on = !g713EnvOn("DC2_G713_NO_BATCH");
    return on;
}

bool g713PipeArmed() { return s_armed; }
bool g713OnExecThread() { return t_onExec; }

// G734: see the api-inc note. Monotonic; the caller keeps its own last-value.
uint64_t g713_exec_busy_ns() { return ring().execTotalNs.load(std::memory_order_relaxed); }
uint64_t g713_exec_idle_ns() { return ring().idleTotalNs.load(std::memory_order_relaxed); }

// ==============================================================================================
// ⭐⭐⭐ G735 — THE EXECUTOR'S REAL CPU, SO THE BOARD STOPS COMPARING OCCUPANCY WITH WORK
// ==============================================================================================
//
// ⛔ THE DEFECT THIS CLOSES. `g713_exec_busy_ns` above is OCCUPANCY: `execTotalNs` brackets a whole
// drain cycle, so every wait INSIDE that cycle — the blocking `glReadPixels`, the row pool's coop
// join, a lock — is counted as busy. Only the ring-empty park lands in `idleTotalNs`.
//
// G734 added that column to the acceptance board and declared the executor the pole on 12 of 18
// windows, against `[G182:ee] cpuMs/f`, which is a `GetThreadTimes` delta — i.e. REAL CPU. A board
// with one work column and two occupancy columns (`[G303:vu1w] busyMs/f` is occupancy too, by
// G434/G476) cannot name a pole, and G735's PC sample of this thread measured it at **46.2 %
// genuine wait**: re-ranking on own CPU puts the executor at the pole on ZERO of those 12 windows.
//
// This makes the column measurable instead of inferred. `GetThreadTimes` on a duplicated handle to
// the executor, read on the board's own window cadence (~2 reads/second), is the SAME instrument
// `[G182:ee]` uses — so the two columns are finally in the same unit.
//
// ⚠️ The handle is duplicated at thread start because `GetCurrentThread()` is a pseudo-handle valid
// only on the calling thread, and this accessor runs on the EE thread at the frame boundary.
// Returns 0 when the pipeline is not armed or the handle could not be taken, which the reporter
// treats as "no column" exactly as it already does for `execMs/f == 0`.
// ⭐ Generalised to a SLOT table so the board can get a work column for every worker thread, not
// just this one. `[G303:vu1w] busyMs/f` is occupancy too (G434/G476) — and the VU1 worker's own
// entry comment says it "has never had a sample distribution, only occupancy timers" — so a board
// that fixes only the executor still ranks one work column against one occupancy column.
//
// Slots: 0 = G713 executor, 1 = VU1 (MTVU) worker, 2 = GS parse worker.
// This TU hosts them because it is COLD and already carries <windows.h>; the capture is one call
// from each thread's own entry, and the read is ~2/second from the EE frame boundary.
#if defined(_WIN32)
std::atomic<void *> g_g735ThreadHandle[kG735ThreadSlots]{};

void g735CaptureThreadHandle(int slot)
{
    if (slot < 0 || slot >= kG735ThreadSlots)
        return;
    void *dup = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                        THREAD_QUERY_INFORMATION, FALSE, 0) != 0)
        g_g735ThreadHandle[slot].store(dup, std::memory_order_release);
}

uint64_t g735ThreadCpuNs(int slot)
{
    if (slot < 0 || slot >= kG735ThreadSlots)
        return 0ull;
    void *h = g_g735ThreadHandle[slot].load(std::memory_order_acquire);
    if (h == nullptr)
        return 0ull;
    FILETIME cr, ex, kt, ut;
    if (!GetThreadTimes(reinterpret_cast<HANDLE>(h), &cr, &ex, &kt, &ut))
        return 0ull;
    const uint64_t k = (static_cast<uint64_t>(kt.dwHighDateTime) << 32) | kt.dwLowDateTime;
    const uint64_t u = (static_cast<uint64_t>(ut.dwHighDateTime) << 32) | ut.dwLowDateTime;
    return (k + u) * 100ull; // FILETIME is 100 ns units -> ns
}
#else
void g735CaptureThreadHandle(int) {}
uint64_t g735ThreadCpuNs(int) { return 0ull; }
#endif

void g735ExecCaptureThreadHandle() { g735CaptureThreadHandle(kG735SlotExec); }
uint64_t g713_exec_cpu_ns() { return g735ThreadCpuNs(kG735SlotExec); }

bool g713PipeStart()
{
    static bool s_done = false;
    if (s_done)
        return s_armed;
    s_done = true;
    if (!g713PipeEnabled())
        return false;
    G713Ring &r = ring();
    if (!r.raster.execBatch || !r.raster.flushOpen || !r.gs.xfer || !r.gs.image)
    {
        std::fprintf(stderr, "[G713:pipe] REFUSED: hooks missing (batch=%d flush=%d xfer=%d img=%d)\n",
                     r.raster.execBatch ? 1 : 0, r.raster.flushOpen ? 1 : 0,
                     r.gs.xfer ? 1 : 0, r.gs.image ? 1 : 0);
        std::fflush(stderr);
        return false;
    }
    const uint32_t mb = g713EnvU32("DC2_G713_RING_MB", 24u);
    r.payCap = static_cast<uint64_t>(mb) * 1024u * 1024u;
    r.pay.assign(static_cast<size_t>(r.payCap), 0u);
    s_serial = g713PipeSerialMode();
    s_armed = true;
    if (!s_serial)
        r.thread = std::thread(g713ExecLoop);
    // G725: the pipeline is default ON, so "ARMED" no longer proves an env flag was set. Print
    // WHICH decision armed it, and keep both legacy positive-arm literals alive in the image so
    // `grep -c DC2_G721_PIPE dc2_runner.exe` still answers "is this mechanism compiled in".
    const bool legacyArm = g713EnvOn("DC2_G713_PIPE") || g713EnvOn("DC2_G721_PIPE");
    std::fprintf(stderr, "[G713:pipe] ARMED mode=%s arm=%s ringMB=%u nodes=%u\n",
                 s_serial ? "SERIAL(oracle)" : "THREADED",
                 legacyArm ? "explicit" : "default", mb, kNodeCap);
    std::fflush(stderr);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Parse-side bridges (forwarders, so the GS TU never names the rasterizer TU's statics)
// ---------------------------------------------------------------------------------------------

bool g713RasterCloseForUpload(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                              uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    const G713RasterHooks &h = ring().raster;
    return h.closeForUpload && h.closeForUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
}

bool g713RasterCloseForLocalCopy(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax,
                                 uint32_t ssay, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                                 uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh)
{
    const G713RasterHooks &h = ring().raster;
    return h.closeForLocalCopy && h.closeForLocalCopy(sbp, sbw, spsm, ssax, ssay,
                                                      dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
}

void g713RasterFlushOpen()
{
    const G713RasterHooks &h = ring().raster;
    if (h.flushOpen)
        h.flushOpen();
}

// ---------------------------------------------------------------------------------------------
// Posts
// ---------------------------------------------------------------------------------------------

void g713PostXfer(const G713XferSnap &s)
{
    G713Ring &r = ring();
    G713Node &n = g713Acquire(r);
    n.kind = kNodeXfer;
    n.snap = s;
    n.ptr = nullptr;
    n.call = nullptr;
    n.payLen = 0;
    g713Publish(r, kNodeXfer);
}

void g713PostImage(const G713XferSnap &s, const uint8_t *data, uint32_t bytes)
{
    if (bytes == 0u || data == nullptr)
        return;
    G713Ring &r = ring();
    if (static_cast<uint64_t>(bytes) > r.payCap / 2u)
    {
        // Pathological single transfer: split it. `processImageData` is chunk-reentrant (it carries
        // m_hwregX/Y across calls), so a split is behaviour-identical to the guest's own chunking.
        uint32_t off = 0;
        // Preserve whole 24-bit pixels as well as ordinary 16/32-bit pixels.
        const uint32_t step = static_cast<uint32_t>(r.payCap / 4u / 12u * 12u);
        G713XferSnap continuation = s;
        while (off < bytes)
        {
            const uint32_t nb = (bytes - off) < step ? (bytes - off) : step;
            g713PostImage(continuation, data + off, nb);
            continuation.resetProgress = 0u;
            off += nb;
        }
        return;
    }
    // A payload never wraps: pad to the end of the byte ring if it will not fit contiguously.
    uint64_t off = r.payHead;
    const uint64_t idx = off % r.payCap;
    if (idx + bytes > r.payCap)
        off += (r.payCap - idx);
    for (;;)
    {
        if ((off + bytes) - r.payTailPub.load(std::memory_order_acquire) <= r.payCap)
            break;
        r.blockedPay.fetch_add(1u, std::memory_order_relaxed);
        if (s_serial)
        {
            g713DrainInline(r);
            continue;
        }
        r.space.wait([&] {
            return r.stop.load(std::memory_order_relaxed) ||
                   off + bytes - r.payTailPub.load(std::memory_order_acquire) <= r.payCap;
        });
    }
    std::memcpy(r.pay.data() + (off % r.payCap), data, bytes);
    r.payHead = off + bytes;

    G713Node &n = g713Acquire(r);
    n.kind = kNodeImage;
    n.snap = s;
    n.payOff = off;
    n.payLen = bytes;
    n.ptr = nullptr;
    n.call = nullptr;
    if (g713PipeStatOn()) r.payBytes.fetch_add(bytes, std::memory_order_relaxed);
    g713Publish(r, kNodeImage);
}

void g713PostBatch(void *closedBatch)
{
    G713Ring &r = ring();
    G713Node &n = g713Acquire(r);
    n.kind = kNodeBatch;
    n.ptr = closedBatch;
    n.call = nullptr;
    n.payLen = 0;
    g713Publish(r, kNodeBatch);
}

void g713CallOnExec(void (*fn)(void *), void *arg)
{
    if (fn == nullptr)
        return;
    G713Ring &r = ring();
    if (t_onExec)
    {
        fn(arg); // already on the exec thread (a nested edge): run it directly, never deadlock
        return;
    }
    // G726: count the round trip BEFORE paying for it. See the adaptive-fallback note above.
    r.callsFrame.fetch_add(1u, std::memory_order_relaxed);
    G713Node &n = g713Acquire(r);
    n.kind = kNodeCall;
    n.call = fn;
    n.ptr = arg;
    n.payLen = 0;
    g713Publish(r, kNodeCall);
    g713Sync();
}

// ==============================================================================================
// ⭐⭐⭐ G740 — THE SAME NODE, WITHOUT THE ROUND TRIP.
//
// `g713CallOnExec` above blocks, and that is correct for every caller that lets the callee read
// live parse-thread state. The FRAME BOUNDARY is not such a caller: under G412 its presentation
// registers are snapshotted on the EE thread before the barrier is enqueued, and everything else
// the closure touches (`g_g144List`, `g_g260Graph`, guest VRAM, GL) is already exec-private under
// G721. So its only reason to block was to bound pipeline depth — which `m_pendingFrameMarkers`
// already bounds independently, on the EE thread.
//
// MEASURED, `dungeon1`, shipped-config binary: `[G713:pipe] syncWaits/f = 1.00 syncMs/f = 12.164`
// against `[G734:exec] idleMs/f = 14.5` and a 32 ms frame. The parse thread spends 38 % of the
// frame parked at that ONE sync while the executor drains, and the executor then idles while the
// parse thread catches up — a two-stage ping-pong, not a pipeline. `[G412:pipeline]
// successor = 59-60/60` proves the parse thread had a queued window to work on every time.
//
// ⛔ A CALLEE POSTED THIS WAY MUST OWN EVERYTHING IT TOUCHES. It runs an unbounded time later,
// with the parse thread already inside the next frame. Do not use it for the non-deferrable
// inline primitive (which reads `gs` live) — that one must keep `g713CallOnExec`.
//
// SERIAL mode is unaffected: `g713Publish` drains inline, so the callee still runs before this
// returns and the oracle stays byte-identical.
// ==============================================================================================
void g713PostCall(void (*fn)(void *), void *arg)
{
    if (fn == nullptr)
        return;
    G713Ring &r = ring();
    if (t_onExec)
    {
        fn(arg); // already on the exec thread (a nested edge): run it directly, never deadlock
        return;
    }
    // NOT counted in `callsFrame`: that census exists to decide whether the BLOCKING round trip
    // rate makes threading a loss (G726's adaptive fallback), and this post does not block.
    G713Node &n = g713Acquire(r);
    n.kind = kNodeCall;
    n.call = fn;
    n.ptr = arg;
    n.payLen = 0;
    g713Publish(r, kNodeCall);
}

void g713Sync()
{
    G713Ring &r = ring();
    if (!s_armed || t_onExec)
        return;
    if (s_serial)
    {
        g713DrainInline(r);
        return;
    }
    const uint64_t want = r.headPub.load(std::memory_order_relaxed);
    if (r.tailPub.load(std::memory_order_acquire) >= want)
        return;
    const bool stat = g713PipeStatOn();
    const auto t0 = stat ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // ⭐⭐ G726: SPIN BEFORE PARKING, because on a round-trip-dense route the PARK is the cost.
    //
    // `Event::wait` ends in `atomic<bool>::wait`, i.e. a futex park plus a wake on the other side.
    // On `dungeon1` that is paid ONCE per frame and is free. On `map15` the blocking bypass fires
    // **1060 times per frame** (local->host readbacks and non-deferrable primitives), and
    // `[G713:pipe]` measured `syncMs/f = 21.672` across exactly `syncWaits/f = 1060.00` - about
    // **20 microseconds per handoff**, which is park/wake latency, not work. That single term is
    // the whole +11.31 ms/f `map15:sky` board regression.
    //
    // The node being waited on is usually tiny (one readback bypass), so the exec thread normally
    // publishes within a few microseconds. Spinning briefly first converts almost every one of
    // those 1060 parks into a load, and costs nothing on routes that only sync once a frame.
    static const uint32_t s_spin = g713EnvU32("DC2_G726_SYNC_SPIN", 8192u);
    bool got = false;
    for (uint32_t i = 0; i < s_spin; ++i)
    {
        if (r.tailPub.load(std::memory_order_acquire) >= want ||
            r.stop.load(std::memory_order_relaxed))
        {
            got = true;
            break;
        }
        _mm_pause();
    }
    while (!got && !r.stop.load(std::memory_order_relaxed) &&
           r.tailPub.load(std::memory_order_acquire) < want)
        r.space.wait([&] {
            return r.stop.load(std::memory_order_relaxed) ||
                   r.tailPub.load(std::memory_order_acquire) >= want;
        });
    if (stat)
    {
        r.syncWaits.fetch_add(1u, std::memory_order_relaxed);
        r.syncNs.fetch_add(static_cast<uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t0).count()),
                           std::memory_order_relaxed);
    }
}

void g713PipeReport()
{
    if (!g713PipeStatOn() || !s_armed)
        return;
    static uint64_t s_n = 0;
    if ((++s_n % 240ull) != 0ull)
        return;
    G713Ring &r = ring();
    const double f = 240.0;
    std::fprintf(stderr,
                 "[G713:pipe] frames=%llu mode=%s posted/f=%.0f (xfer=%.1f img=%.1f batch=%.1f "
                 "call=%.1f) payMB/f=%.2f | syncWaits/f=%.2f syncMs/f=%.3f maxDepth=%llu "
                 "blockedPost=%llu blockedPay=%llu execMs/f=%.3f\n",
                 static_cast<unsigned long long>(s_n), s_serial ? "SERIAL" : "THREADED",
                 r.posted.exchange(0, std::memory_order_relaxed) / f,
                 r.byKind[kNodeXfer].exchange(0, std::memory_order_relaxed) / f,
                 r.byKind[kNodeImage].exchange(0, std::memory_order_relaxed) / f,
                 r.byKind[kNodeBatch].exchange(0, std::memory_order_relaxed) / f,
                 r.byKind[kNodeCall].exchange(0, std::memory_order_relaxed) / f,
                 r.payBytes.exchange(0, std::memory_order_relaxed) / (f * 1048576.0),
                 r.syncWaits.exchange(0, std::memory_order_relaxed) / f,
                 r.syncNs.exchange(0, std::memory_order_relaxed) / (1.0e6 * f),
                 static_cast<unsigned long long>(r.maxDepth.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long long>(r.blockedPost.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(r.blockedPay.load(std::memory_order_relaxed)),
                 r.execNs.exchange(0, std::memory_order_relaxed) / (1.0e6 * f));
    // ⭐ G734: the executor's WORK / WAIT split on the same window. `execMs/f + idleMs/f` should
    // approach the frame span; the residue is the consume loop's own overhead. An executor whose
    // idleMs/f is near zero is SATURATED, and every wait upstream of it (the parse thread's
    // syncMs/f, the EE thread's m_cvFrameSlot park) is conservation, not a removable throttle.
    // ⛔ G743 (Rule G736-1): publish the ARM beside the value. Before G743 these two accumulators
    // were gated on `DC2_G182_EE_STAT` while this line printed under `DC2_G713_PIPE_STAT`, so a
    // disarmed instrument printed `idleMs/f=0.000` — which the note above reads as "SATURATED".
    std::fprintf(stderr,
                 "[G734:exec] idleMs/f=%.3f idleWaits/f=%.2f avail=%s\n",
                 r.idleNs.exchange(0, std::memory_order_relaxed) / (1.0e6 * f),
                 r.idleWaits.exchange(0, std::memory_order_relaxed) / f,
                 g713PipeStatOn() ? "armed" : "DISARMED-structural-zero");
    // G724: the same window, split by node kind. These four sum to `execMs/f` by construction.
    std::fprintf(stderr,
                 "[G713:kind] xferMs/f=%.3f imgMs/f=%.3f batchMs/f=%.3f callMs/f=%.3f\n",
                 r.nsByKind[kNodeXfer].exchange(0, std::memory_order_relaxed) / (1.0e6 * f),
                 r.nsByKind[kNodeImage].exchange(0, std::memory_order_relaxed) / (1.0e6 * f),
                 r.nsByKind[kNodeBatch].exchange(0, std::memory_order_relaxed) / (1.0e6 * f),
                 r.nsByKind[kNodeCall].exchange(0, std::memory_order_relaxed) / (1.0e6 * f));
    std::fflush(stderr);
}

// ==============================================================================================
// ⭐⭐⭐ G726 - THE PIPELINE MUST TURN ITSELF OFF ON ROUND-TRIP-DENSE ROUTES
//
// THE MEASUREMENT THAT FORCED THIS. The G726 canonical board is -2.062 ms/f in the mean and moves
// the pole off GS on 21 of 23 windows, but ONE window regressed catastrophically:
// `map15:sky` 24.59 -> 35.90 ms/f (+46%), with `gsFront` 17.69 -> 28.76 while backend, VU1 and
// kicks were all flat. Same binary, `map15`, armed vs `DC2_G713_NO_PIPE=1`:
//
//     ARMED        call/f = 1060.0   syncWaits/f = 1060.00   syncMs/f = 21.672   frame 30.4-31.1
//     ROLLED BACK                                                                frame 18.5-18.7
//
// `dungeon1` takes the blocking bypass **1.0 times per frame**; `map15` takes it **1060 times**.
// Each one is a full parse->exec round trip, and the parse thread spends 21.7 ms/f inside them.
// The pipeline's whole premise - that the ONLY blocking edge is the frame boundary - is true of
// every perf route in the corpus and FALSE of the RTT/page-ownership route, which is dense in
// local->host readbacks and non-deferrable primitives.
//
// THE FIX IS NOT A NEW MECHANISM. `DC2_G713_SERIAL` already exists and is the ORACLE mode: the ring
// is used but drained INLINE at every post, so nothing crosses a thread and the result is
// pixel-identical by construction (it is what G721 gated the decomposition with). A route whose
// blocking rate makes threading a loss should simply run in that mode.
//
// WHY THE FRAME BOUNDARY IS THE SAFE PLACE TO FLIP. The boundary call is itself a blocking bypass:
// by the time this runs on the parse thread, `g713Sync` has returned, the ring is drained and the
// exec thread is parked with no work. Setting `s_serial` there cannot race a node in flight.
//
// HYSTERESIS, so a single dense frame does not flip a good route: the rate must exceed the
// threshold on `kAdaptFrames` CONSECUTIVE frames. The flip is one-way - a route that has proven
// round-trip dense does not get re-tested every frame, and one-way means the decision can never
// oscillate mid-scene.
//
// Threshold: `DC2_G726_ADAPT_CALLS` (default 64). `dungeon1` measures 1.0 and `map15` 1060, so any
// value in between separates them; 64 is two orders above the good case and one below the bad one.
// Rollback: `DC2_G726_NO_ADAPT=1` restores the unconditional threading this board measured.
// ==============================================================================================
// ⛔⛔⛔ MEASURED HARMFUL, SO IT IS OFF BY DEFAULT. Latching to SERIAL at runtime made `map15`
// **63-64 ms/f** - worse than either arm it chooses between (30.4 threaded, 18.5 disabled) - and
// stopping+joining the exec thread first did not change that. The discriminating run: SERIAL from
// PROCESS START is 20.9-23.8 ms/f on the same route and binary. So SERIAL is fine and the MID-RUN
// CONSUMER HANDOFF is what costs 40 ms/f. That is not surprising in hindsight - `s_serial` is read
// by `g713_raster_arm` to decide whether the exec thread is ever created, and several structures
// (notably G712 prep-ahead's lock-free free list, whose open/consume/release are all documented as
// happening on ONE thread) are built around the consumer identity never changing.
//
// The direction is kept, disarmed, because the DIAGNOSIS it produced is what led to the real fix
// (the spin in `g713Sync`): default threshold is "never", and `DC2_G726_ADAPT_CALLS=<n>` arms it
// for anyone who wants to re-measure a corrected transition.
uint32_t g713AdaptThreshold()
{
    static const uint32_t v = g713EnvOn("DC2_G726_NO_ADAPT") ? 0xFFFFFFFFu
                                                             : g713EnvU32("DC2_G726_ADAPT_CALLS", 0xFFFFFFFFu);
    return v;
}

void g713PipeAdaptTick()
{
    // Runs once per frame boundary on the parse thread, ring drained. Cheap by construction: one
    // relaxed exchange and a compare, on a thread that has just finished a blocking sync.
    if (!s_armed || s_serial)
        return;
    const uint32_t limit = g713AdaptThreshold();
    G713Ring &r = ring();
    const uint32_t calls = r.callsFrame.exchange(0u, std::memory_order_relaxed);
    if (calls <= limit)
    {
        s_adaptHot = 0u;
        return;
    }
    constexpr uint32_t kAdaptFrames = 8u;
    if (++s_adaptHot < kAdaptFrames)
        return;
    // ⛔⛔⛔ THE FLAG IS NOT THE TRANSITION. The first version of this simply set `s_serial` and
    // measured `map15` at 63-66 ms/f - WORSE than either arm it was meant to choose between (30.4
    // threaded, 18.5 rolled back). The reason is structural: in a real SERIAL run `s_serial` is
    // true BEFORE `g713_raster_arm` decides whether to create the thread, so no exec thread ever
    // exists. Latching it at runtime leaves the exec thread alive and looping on `r.tailC`, so the
    // parse thread's inline drain and the exec thread become TWO CONSUMERS on a ring that is
    // single-consumer by construction. Retire the thread first, then latch.
    g713Sync();                                   // 1. drain everything, thread still doing the work
    r.stop.store(true, std::memory_order_release); // 2. tell the exec loop to exit
    r.work.notify();
    r.space.notify();
    if (r.thread.joinable())
        r.thread.join();                          // 3. it is gone; there is now exactly one consumer
    r.stop.store(false, std::memory_order_release); // 4. `stop` only ever gated thread waits
    s_serial = true;                              // 5. every later post drains inline, in order
    std::fprintf(stderr,
                 "[G713:pipe] ADAPT -> SERIAL: %u blocking calls/frame over %u consecutive frames "
                 "(limit %u). Round-trip-dense route; threading is a loss here.\n",
                 calls, kAdaptFrames, limit);
    std::fflush(stderr);
}

void g713PipeStop()
{
    G713Ring &r = ring();
    if (!s_armed)
        return;
    // ⚠️ G726: `s_serial` may have been latched by the adaptive tick AFTER the exec thread was
    // started, so "serial" no longer implies "no thread to join". Joining is keyed on the thread
    // itself. The old early-return on `s_serial` would have leaked a parked thread here.
    if (s_serial && !r.thread.joinable())
        return;
    g713Sync();
    r.stop.store(true, std::memory_order_release);
    r.work.notify();
    r.space.notify();
    if (r.thread.joinable())
        r.thread.join();
}
