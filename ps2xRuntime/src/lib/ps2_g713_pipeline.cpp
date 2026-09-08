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
// ARMS
//   DC2_G713_PIPE=1        master arm (default OFF)
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "ps2_g713_pipeline_api.inc"

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

    uint64_t head = 0;    // producer-owned: next slot to write
    uint64_t payHead = 0; // producer-owned: next payload byte to write
    uint64_t tailC = 0;   // consumer-owned
    uint64_t payTailC = 0;

    std::atomic<uint64_t> headPub{0};
    std::atomic<uint64_t> tailPub{0};
    std::atomic<uint64_t> payTailPub{0};

    std::mutex mtx;
    std::condition_variable cvWork, cvSpace;
    std::atomic<bool> stop{false};
    std::thread thread;

    G713RasterHooks raster{};
    G713GsHooks gs{};
    void (*threadInit)() = nullptr;

    std::atomic<uint64_t> posted{0}, byKind[kNodeKindN]{}, blockedPost{0}, blockedPay{0},
        syncWaits{0}, syncNs{0}, payBytes{0}, maxDepth{0};
};

G713Ring &ring()
{
    static G713Ring r;
    return r;
}

bool s_serial = false;
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
    while (r.tailC != head)
    {
        G713Node &n = r.node[r.tailC & (kNodeCap - 1u)];
        g713RunNode(r, n);
        if (n.kind == kNodeImage)
            r.payTailC = n.payOff + n.payLen;
        ++r.tailC;
        if (threaded)
        {
            r.tailPub.store(r.tailC, std::memory_order_release);
            r.payTailPub.store(r.payTailC, std::memory_order_release);
            r.cvSpace.notify_all(); // the producer may be parked on ring or payload space
        }
        else
        {
            r.tailPub.store(r.tailC, std::memory_order_relaxed);
            r.payTailPub.store(r.payTailC, std::memory_order_relaxed);
        }
    }
}

void g713ExecLoop()
{
    G713Ring &r = ring();
    t_onExec = true;
    if (r.threadInit)
        r.threadInit();
    for (;;)
    {
        uint64_t head = r.headPub.load(std::memory_order_acquire);
        if (r.tailC == head)
        {
            std::unique_lock<std::mutex> lk(r.mtx);
            r.cvWork.wait(lk, [&] {
                return r.stop.load(std::memory_order_relaxed) ||
                       r.headPub.load(std::memory_order_acquire) != r.tailC;
            });
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
        std::unique_lock<std::mutex> lk(r.mtx);
        r.cvWork.notify_one();
        r.cvSpace.wait(lk, [&] {
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
    r.posted.fetch_add(1u, std::memory_order_relaxed);
    r.byKind[kind].fetch_add(1u, std::memory_order_relaxed);
    if (s_serial)
    {
        g713DrainInline(r);
        return;
    }
    const uint64_t depth = r.head - r.tailPub.load(std::memory_order_relaxed);
    if (depth > r.maxDepth.load(std::memory_order_relaxed))
        r.maxDepth.store(depth, std::memory_order_relaxed);
    r.cvWork.notify_one();
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
    static const bool on = g713EnvOn("DC2_G713_PIPE") && !g713EnvOn("DC2_G713_NO_PIPE");
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
    std::fprintf(stderr, "[G713:pipe] ARMED mode=%s ringMB=%u nodes=%u\n",
                 s_serial ? "SERIAL(oracle)" : "THREADED", mb, kNodeCap);
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
        const uint32_t step = static_cast<uint32_t>(r.payCap / 4u);
        while (off < bytes)
        {
            const uint32_t nb = (bytes - off) < step ? (bytes - off) : step;
            g713PostImage(s, data + off, nb);
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
        std::unique_lock<std::mutex> lk(r.mtx);
        r.cvWork.notify_one();
        r.cvSpace.wait_for(lk, std::chrono::milliseconds(2));
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
    r.payBytes.fetch_add(bytes, std::memory_order_relaxed);
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
    G713Node &n = g713Acquire(r);
    n.kind = kNodeCall;
    n.call = fn;
    n.ptr = arg;
    n.payLen = 0;
    g713Publish(r, kNodeCall);
    g713Sync();
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
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lk(r.mtx);
        r.cvWork.notify_one();
        r.cvSpace.wait(lk, [&] {
            return r.stop.load(std::memory_order_relaxed) ||
                   r.tailPub.load(std::memory_order_acquire) >= want;
        });
    }
    r.syncWaits.fetch_add(1u, std::memory_order_relaxed);
    r.syncNs.fetch_add(static_cast<uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - t0).count()),
                       std::memory_order_relaxed);
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
                 "blockedPost=%llu blockedPay=%llu\n",
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
                 static_cast<unsigned long long>(r.blockedPay.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

void g713PipeStop()
{
    G713Ring &r = ring();
    if (!s_armed || s_serial)
        return;
    g713Sync();
    {
        std::lock_guard<std::mutex> lk(r.mtx);
        r.stop.store(true, std::memory_order_relaxed);
    }
    r.cvWork.notify_all();
    if (r.thread.joinable())
        r.thread.join();
}
