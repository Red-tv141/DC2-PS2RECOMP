#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "ps2_frame_dump.h"
#include "runtime/ps2_gs_gpu.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"

// G438 derivative-probe arm selector (defined in ps2_gs_rasterizer_parts/g419_ab_instrument.inc).
// -1 = this probe is not the selected DC2_G419_AB lever, 0 = control frame, 1 = candidate frame.
extern int g438SlowArm(int which);

// G478 VU1-worker PC sampler (defined in ps2_gif_arbiter_parts/g446_gsworker_pcsample.inc).
// No-op unless DC2_G478_VU1PROF=1.
extern void g478RegisterVu1WorkerThread();

// G478 kick-item pool paired gate (DC2_G419_AB=kickpool). -1 = not selected, 0 = control, 1 = cand.
extern int g478KickPoolArm();

// G482 memo-key PRICE arm (DC2_G419_AB=memohash). -1 = not selected, 0 = control, 1 = candidate.
extern int g482MemoHashArm();

// G482 VU-RAM write-coalescing arm (DC2_G419_AB=wcoal). -1 = not selected, 0 = control, 1 = cand.
extern int g482WriteCoalesceArm();

// G482 publish-notify-skip arm (DC2_G419_AB=notifyskip). -1 = not selected, 0 = control, 1 = cand.
extern int g482NotifySkipArm();

// G483 pair-loop iteration count of the LAST VU1Interpreter::run() on THIS thread (defined in
// ps2_vu1_parts/vu1_g483_run_cycles.inc). Consumed only under DC2_G441_KICK=1.
extern uint64_t dc2_g483_last_run_cycles();
extern uint32_t dc2_g483_last_stop_reason();

// G482 COMBINED promotion arm (DC2_G419_AB=g482both) — both levers switched together, exactly the
// shipped default against exactly the full rollback (G447: two levers on one serialized edge do
// not add, so the promotion number must come from one arm).


#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <immintrin.h>

namespace ps2_stubs
{
    void resetSifState();
}

void (*g_g7_poll_live_pad_hook)() = nullptr;
void (*g_g55_title_draw_probe_hook)(uint8_t *rdram) = nullptr;
bool (*g_f66_drive_dungeon_pad_hook)(uint32_t) = nullptr;

// ============================================================================
// G297 MTVU — run the VU1 interpreter on a dedicated worker thread (default-off
// DC2_G297_MTVU=1). The pole re-ranked to EE-compute-bound (VU1 ~62 ms ≈ 65% of the
// EE thread's ~85 ms, co-balanced with the GS worker ~62 ms). Moving VU1 off the EE
// makes a 3-stage pipeline EE → VU1-worker → GS-worker; the frame floors at the
// balanced VU1/GS stage (~62 ms) instead of the serial EE (~85). Ceiling ~+35-40% FPS.
//
// Correctness (bit-exact by construction):
//  - The interpreter is UNCHANGED; it just runs on the worker over an immutable per-kick
//    snapshot of VU1 code+data. m_vu1 is touched only by the worker (the EE no longer calls it).
//  - Each XGKICK is captured (g_g297KickCapture, ps2_vu1.cpp) into the kick's private Path1 list
//    instead of the (single-writer, EE-only) GifArbiter.
//  - The GifArbiter re-sorts every drain WINDOW by GS path priority (stable within a path), so
//    cross-path submission interleaving is irrelevant. Correctness only requires each kick's Path1
//    packets to land in the SAME window as baseline, in kick order. The EE seals a per-window kick
//    BATCH at each drain() (1:1 with the enqueued window); the MTGS worker, in processWindow, pops
//    that batch, waits for its kicks, and merges their Path1 packets before the sort — identical
//    packets, identical window, identical order → bit-exact.
//  - Pipelines: the EE never waits for the worker at kick issue; the MTGS worker waits per-window
//    only if VU1 has not caught up (natural backpressure). frameDrain (mgEndFrame) transitively
//    waits for VU1 via the worker.
// Requires MTGS (rides its window sort/drain). Incompatible with VU diagnostics that read shared
// gs/memory mid-execute (all default-off) → those force the synchronous fallback.
// ============================================================================
extern thread_local std::vector<std::vector<uint8_t>> *g_g297KickCapture; // defined in ps2_vu1.cpp
extern thread_local uint32_t g_g297KickTop;                              // defined in ps2_vu1.cpp
extern bool g150_mtgs_enabled();                                          // defined in ps2_gif_arbiter.cpp
extern void dc2_g410_vu_code_written(const uint8_t *codeBase, uint32_t offset, uint32_t sizeBytes);
extern std::atomic<uint64_t> g_dc2PresentTick;  // global scope (defined in dc2_game_override.cpp); this
                                                // .inc is included before runtime_dispatch_and_memory.inc's
                                                // own extern, and it must NOT be declared inside the anon
                                                // namespace below (G260 anon-namespace-linkage trap).

namespace {

// G301: one ordered VU-RAM write (VIF UNPACK / MPG / fog patch) captured on the EE and replayed by
// the worker onto its PERSISTENT VU RAM before the kick runs. This is the fix for the G297 per-kick
// SNAPSHOT data-model bug (pinned in G301): the snapshot discarded the interpreter's own VU-data-RAM
// writes (SQ/SQI/SQD), so a later kick reading that data got stale/zero → dropped 3D geometry.
// Replaying deltas onto a resident worker buffer makes cross-kick VU1→VU1 writes persist.
struct G297VuWrite
{
    uint32_t off;   // byte offset into worker VU code (isCode) or data RAM
    uint32_t len;   // payload byte count
    uint8_t isCode; // 1 = code RAM (MPG), 0 = data RAM (UNPACK/fog)
};

struct G297KickItem
{
    std::vector<G297VuWrite> writes;               // ordered VU-RAM writes preceding this kick
    std::vector<uint8_t> writeBytes;               // concatenated payloads, in `writes` order
    uint32_t startPC = 0u;
    uint32_t itop = 0u;
    uint32_t top = 0u;       // VIF1 double-buffer DATA top (XTOP), snapshotted at MSCAL/MSCNT
    uint32_t budget = 0u;
    bool isResume = false;
    std::vector<std::vector<uint8_t>> packets;     // XGKICK Path1 output (worker-filled)
    std::atomic<bool> ready{false};
};
using G297KickPtr = std::shared_ptr<G297KickItem>;

// ---------------------------------------------------------------------------
// G478 kick-item RECYCLING POOL (MEASURED NEUTRAL — default OFF, opt-in DC2_G478_KICKPOOL=1).
//
// RESULT: the paired gate `DC2_G419_AB=kickpool` (hold=16, 3,000 frames) read **+0.065 ms/f
// (0.26%, t = 0.76)** — i.e. no win, trending to zero as n grew (+0.40 -> +0.26 -> +0.12 -> +0.08
// -> +0.065). The ~4% allocator rows in the VU1 sample are therefore NOT the item; they belong to
// the per-XGKICK inner packet buffers, which this pool cannot recycle because
// collectWindowPackets MOVES them out to the GS worker's window list and they are freed there.
// Kept, default-off, with its paired gate so it can be re-priced if packet ownership changes.
//
// The G478 premise gate re-ranked the VU1 worker to CO-POLE (a +1.23 ms/f spin moved the frame
// +0.99, t = 11.4, against the GS worker's 0.76), and the first PC sample ever taken of that
// thread found it 82.75% inside VU1Interpreter::run with ~0% blocked — and ~4% spread over
// RtlAllocateHeap / malloc / operator new / _Emplace_reallocate / _Uninitialized_move /
// _Destroy_range / memmove. That residue is this item: MAP-0 issues ~1,230 kicks/frame and each
// one was a fresh `make_shared<G297KickItem>` whose three vectors were grown from zero capacity
// and freed again, with the ALLOCATION on one thread and the FREE on another (the item's last
// reference dies in the GS worker's collectWindowPackets batch).
//
// EXACTNESS. A recycled item is reset to exactly the state `make_shared` produced: `writes`,
// `writeBytes` and `packets` cleared (size 0) and `ready` false. Only CAPACITY survives, which no
// consumer can observe. The two payload vectors are `swap`ped with the EE's pending accumulators
// rather than moved, so the EE gets the item's old buffer back instead of a capacity-less husk —
// the previous `std::move` + `clear()` left `m_pendWrites` with no capacity, so every noteVuWrite
// after a kick re-grew it from scratch. The pool is a process-lifetime static (never destroyed),
// so an item whose last reference outlives the MTVU object still has a valid home.
struct G478KickPool
{
    std::mutex mtx;
    std::vector<G297KickItem *> free;
};
inline G478KickPool &g478KickPool()
{
    static G478KickPool *s_pool = new G478KickPool(); // intentionally leaked: process lifetime
    return *s_pool;
}
// G480 PROMOTED this default-ON. G478 measured the same pool at +0.065 ms/f (t = 0.76) and left it
// opt-in; that verdict EXPIRED the moment G480's packet pool took the per-XGKICK buffers off the
// same heap (the G445 expiry rule, and the second time this arc has seen a "neutral" flip once the
// edge underneath it moved — cf. G474). Re-priced on the G480 binary the paired gate
// `DC2_G419_AB=kickpool` reads **-0.333 ms/f (t = -5.24, 5/5 windows)**. Two allocation sources
// were CONTENDING for the same allocator: removing either alone left the other to keep the heap
// hot, so neither could show its size. Rollback: DC2_G478_NO_KICKPOOL=1.
// G482: contiguous VU-RAM write coalescing (default ON; see noteVuWrite for the mechanism).
inline bool g482WriteCoalesceOn()
{
    static const bool s_on = (std::getenv("DC2_G482_NO_WCOAL") == nullptr);
    return s_on;
}

// G482: skip the per-kick done-mutex + notify when no thread is waiting.
// REJECTED — MEASURED SLOWER, default OFF (opt-in DC2_G482_NOTIFYSKIP=1, kept as the record).
// The paired gate `DC2_G419_AB=notifyskip` read **+0.528 ms/f (t = +2.66, 0/4 windows)**. The lock
// this removes is UNCONTENDED (the GS worker runs ~1,950 kicks behind and never sleeps here), so
// its acquire/release is a couple of cache-hot atomics and `notify_all` on an empty condition
// variable is a cheap no-op. What replaces it is strictly more expensive: `ready` has to be
// upgraded from `release` to `seq_cst` and the waiter count read `seq_cst`, i.e. TWO full barriers
// per kick on the sole pole thread. Removing a lock is only a win if the lock is contended.
inline bool g482NotifySkipOn()
{
    static const bool s_on = (std::getenv("DC2_G482_NOTIFYSKIP") != nullptr);
    return s_on;
}

inline bool g478KickPoolOn()
{
    static const bool s_on = (std::getenv("DC2_G478_NO_KICKPOOL") == nullptr);
    return s_on;
}
inline G297KickPtr g478AcquireKickItem()
{
    // DC2_G419_AB=kickpool: -1 = not the selected lever (use the shipped default), 0 = control
    // frame (the pre-G478 fresh allocation), 1 = candidate frame (pooled).
    const int abArm = g478KickPoolArm();
    if (abArm == 0 || (abArm < 0 && !g478KickPoolOn()))
        return std::make_shared<G297KickItem>();
    G478KickPool &pool = g478KickPool();
    G297KickItem *raw = nullptr;
    {
        std::lock_guard<std::mutex> lk(pool.mtx);
        if (!pool.free.empty())
        {
            raw = pool.free.back();
            pool.free.pop_back();
        }
    }
    if (raw == nullptr)
        raw = new G297KickItem();
    else
    {
        // Reset to the exact post-construction state; only capacity is carried forward.
        raw->writes.clear();
        raw->writeBytes.clear();
        raw->packets.clear();
        raw->startPC = 0u;
        raw->itop = 0u;
        raw->top = 0u;
        raw->budget = 0u;
        raw->isResume = false;
        raw->ready.store(false, std::memory_order_relaxed);
    }
    return G297KickPtr(raw, [](G297KickItem *p) {
        G478KickPool &p2 = g478KickPool();
        std::lock_guard<std::mutex> lk(p2.mtx);
        if (p2.free.size() < 8192u)
            p2.free.push_back(p);
        else
            delete p;
    });
}

// ---------------------------------------------------------------------------
// G331 kick-redundancy census (DC2_G331_KICKCENSUS=1). EE-thread only (called
// from enqueueKick, the single MSCAL/MSCNT producer). Reads COUNTS, not timings,
// and runs on the mostly-idle EE thread, so its self-cost does NOT perturb the
// VU1/GS worker pole (the thing we want to shrink). Question it answers: of the
// ~1250 kicks/frame at MAP-0, how many are REDUNDANT work?
//   - intra-frame duplicate  = byte-identical kick already issued THIS frame
//                              (multi-pass: same geometry to RTT + display, etc.)
//   - inter-frame duplicate  = byte-identical to a kick in the PREVIOUS frame
//                              (static geometry: transformed output is cacheable)
// A kick's identity is (startPC,itop,top, FNV-1a of writeBytes). Resume (MSCNT)
// kicks carry no writeBytes and depend on persistent VU state, so they are counted
// separately and EXCLUDED from dup detection (they would all collide falsely).
// The writeBytes-only identity ignores un-rewritten persistent VU RAM, so a match
// is NECESSARY-but-not-sufficient for true redundancy => the reported dup fraction
// is an UPPER BOUND. A low number here definitively rules redundancy out; a high
// number is a lead to confirm. (g_dc2PresentTick extern-declared at global scope
// in runtime_dispatch_and_memory.inc; do NOT re-declare it inside this anon namespace.)
static inline void g331KickCensus(uint32_t startPC, uint32_t itop, uint32_t top,
                                  const std::vector<uint8_t> &wb, bool isResume)
{
    static const bool s_on = (std::getenv("DC2_G331_KICKCENSUS") != nullptr);
    if (!s_on)
        return;

    static uint64_t s_lastTick = ~0ull;
    static std::unordered_map<uint64_t, uint32_t> s_frame;     // hash -> count, current frame
    static std::unordered_map<uint64_t, uint32_t> s_prevFrame; // previous frame's hashes
    // window (30-frame) accumulators
    static uint32_t s_winFrames = 0u;
    static uint64_t s_winKicks = 0ull, s_winResume = 0ull;
    static uint64_t s_winDupIntra = 0ull, s_winDupInter = 0ull;
    static uint64_t s_winWriteBytes = 0ull;
    static std::unordered_map<uint32_t, uint64_t> s_winStartPC; // startPC -> count over window

    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    if (tick != s_lastTick)
    {
        // frame boundary: finalize the frame that just ended
        for (const auto &kv : s_frame)
            if (kv.second > 1u)
                s_winDupIntra += (kv.second - 1u);
        s_prevFrame.swap(s_frame);
        s_frame.clear();
        s_lastTick = tick;
        ++s_winFrames;
        if (s_winFrames >= 30u)
        {
            // top-3 startPCs by frequency
            uint32_t t0 = 0u, t1 = 0u, t2 = 0u;
            uint64_t c0 = 0u, c1 = 0u, c2 = 0u;
            for (const auto &kv : s_winStartPC)
            {
                if (kv.second > c0) { t2 = t1; c2 = c1; t1 = t0; c1 = c0; t0 = kv.first; c0 = kv.second; }
                else if (kv.second > c1) { t2 = t1; c2 = c1; t1 = kv.first; c1 = kv.second; }
                else if (kv.second > c2) { t2 = kv.first; c2 = kv.second; }
            }
            const double w = (double)s_winFrames;
            const uint64_t geom = s_winKicks - s_winResume;
            std::fprintf(stderr,
                "[G331:kicks] frames=%u kicks/f=%.0f resume/f=%.0f distinctPC=%zu "
                "dupIntra/f=%.0f (%.1f%%) dupInter/f=%.0f (%.1f%%) avgWB=%.0f | "
                "topPC=[0x%x:%.0f 0x%x:%.0f 0x%x:%.0f]/f\n",
                s_winFrames, (double)s_winKicks / w, (double)s_winResume / w, s_winStartPC.size(),
                (double)s_winDupIntra / w, geom ? 100.0 * (double)s_winDupIntra / (double)geom : 0.0,
                (double)s_winDupInter / w, geom ? 100.0 * (double)s_winDupInter / (double)geom : 0.0,
                s_winKicks ? (double)s_winWriteBytes / (double)s_winKicks : 0.0,
                t0, (double)c0 / w, t1, (double)c1 / w, t2, (double)c2 / w);
            s_winFrames = 0u;
            s_winKicks = s_winResume = 0ull;
            s_winDupIntra = s_winDupInter = 0ull;
            s_winWriteBytes = 0ull;
            s_winStartPC.clear();
        }
    }

    ++s_winKicks;
    s_winWriteBytes += wb.size();
    ++s_winStartPC[startPC];
    if (isResume)
    {
        ++s_winResume;
        return; // resume kicks excluded from dup detection
    }

    // FNV-1a 64 over (startPC,itop,top,writeBytes)
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(&startPC, sizeof(startPC));
    mix(&itop, sizeof(itop));
    mix(&top, sizeof(top));
    if (!wb.empty())
        mix(wb.data(), wb.size());

    if (s_prevFrame.find(h) != s_prevFrame.end())
        ++s_winDupInter;
    ++s_frame[h];
}

// G482: full-identity memo census + memo-key price arm (both default-off). Placed here so it can
// see G297KickItem/VU1State and be called from the worker loop below.
#include "runtime_g482_memo_census.inc"

class G297Mtvu
{
public:
    static G297Mtvu &instance()
    {
        static G297Mtvu s;
        return s;
    }

    ~G297Mtvu() { shutdown(); } // join the worker at exit (a joinable std::thread dtor terminates)

    // Cached: DC2_G297_MTVU=1 AND MTGS on AND no incompatible VU diagnostic.
    static bool activeCached()
    {
        static const bool on = computeActive();
        return on;
    }

    void wire(VU1Interpreter *vu1, PS2Memory *mem, GS *gs)
    {
        m_vu1 = vu1;
        m_mem = mem;
        m_gs = gs;
        if (!m_started)
        {
            m_started = true;
            m_thread = std::thread([this] { worker(); });
        }
    }

    // EE thread (VIF1 interpreter): record one VU-RAM write (UNPACK/MPG/fog) for the CURRENT,
    // not-yet-enqueued kick. Accumulates into an EE-local delta that the next enqueueKick moves into
    // the kick item; the worker replays it onto its persistent VU RAM before running the kick.
    void noteVuWrite(bool isCode, uint32_t off, const uint8_t *src, uint32_t bytes)
    {
        if (!src || bytes == 0u)
            return;
        // G482: COALESCE CONTIGUOUS WRITES. MAP-0 records ~59 writes/kick averaging ~16 bytes
        // ([G441:kick] writes/kick=59.3 bytes/kick=973), because a VIF1 UNPACK run arrives one
        // qword at a time. The worker then replays each one as its own bounds-checked `memcpy` of
        // ~16 bytes plus, for code, a `dc2_g410_vu_code_written` call — ~0.64 us/kick, ~0.79 ms/f
        // ON THE POLE THREAD.
        //
        // The payload bytes are ALREADY concatenated in `writes` order, so two records that are
        // adjacent in the list AND adjacent in VU RAM describe one contiguous copy. Merging them
        // here replays byte-identical content to byte-identical offsets — the merged record's
        // `memcpy` covers exactly the two it replaces, in the same order, on the same buffer.
        //
        // The G480 placement rule: this comparison runs on the EE thread, whose derivative
        // sensitivity is ~0, and it REMOVES work from the VU1 worker, which is the sole pole.
        // Strict contiguity only — an overlapping or out-of-order write keeps its own record, so
        // last-writer-wins ordering is preserved exactly.
        // Rollback: DC2_G482_NO_WCOAL=1   Paired gate: DC2_G419_AB=wcoal
        const int abArm = g482WriteCoalesceArm();
        if (abArm == 1 || (abArm < 0 && g482WriteCoalesceOn()))
        {
            if (!m_pendWrites.empty())
            {
                G297VuWrite &back = m_pendWrites.back();
                if (back.isCode == (isCode ? 1u : 0u) && back.off + back.len == off)
                {
                    back.len += bytes;
                    m_pendWriteBytes.insert(m_pendWriteBytes.end(), src, src + bytes);
                    return;
                }
            }
        }
        m_pendWrites.push_back(G297VuWrite{off, bytes, static_cast<uint8_t>(isCode ? 1u : 0u)});
        m_pendWriteBytes.insert(m_pendWriteBytes.end(), src, src + bytes);
    }

    // EE thread: attach the VU-RAM writes accumulated since the last kick, enqueue to the worker,
    // return immediately. The worker owns/persists VU RAM, so there is no per-kick full snapshot.
    void enqueueKick(uint32_t startPC, uint32_t itop, uint32_t top, uint32_t budget, bool isResume)
    {
        // G478: recycled item (see G478KickPool). SWAP, not move — a move leaves m_pendWrites with
        // zero capacity, so the next kick's noteVuWrite run re-grows it from scratch; the swap hands
        // the EE back the item's previous buffer instead. Both accumulators end EMPTY either way.
        auto item = g478AcquireKickItem();
        item->writes.swap(m_pendWrites);               // EE-local until pushed: swap, never copy
        item->writeBytes.swap(m_pendWriteBytes);
        m_pendWrites.clear();                          // swapped-in buffers → empty + capacity kept
        m_pendWriteBytes.clear();
        item->startPC = startPC;
        item->itop = itop;
        item->top = top;
        item->budget = budget;
        item->isResume = isResume;
        m_curBatch.push_back(item);                    // EE-only accumulator (issue order)
        ++m_kicksEnq;
        g331KickCensus(item->startPC, item->itop, item->top, item->writeBytes, item->isResume);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_work.push_back(item);
        }
        m_cvWork.notify_one();
        // G301 diagnostic: DC2_G301_SYNC_WORKER runs the kick on the worker but BLOCKS the EE until
        // it completes (zero EE/worker overlap). Retained as a bisect tool — it forces the worker-owned
        // VU-RAM model to run without any EE/worker concurrency, isolating data-model correctness from
        // pipeline/overlap effects. Default off; the promoted path overlaps.
        static const bool s_syncWorker = (std::getenv("DC2_G301_SYNC_WORKER") != nullptr);
        if (s_syncWorker)
        {
            m_doneWaiters.fetch_add(1, std::memory_order_seq_cst); // G482: see collectWindowPackets
            std::unique_lock<std::mutex> lk(m_doneMtx);
            m_cvDone.wait(lk, [&] { return m_stop || item->ready.load(std::memory_order_acquire); });
            m_doneWaiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    // EE thread: are there kicks issued since the last window handoff? (peek — m_curBatch is
    // EE-only, no lock needed). drain() uses this to decide whether a window must be materialized
    // even when it carries no arbiter packets (a geometry-only VIF chain: MSCAL kicks, no PATH2/3).
    bool hasPendingKicks() const { return !m_curBatch.empty(); }

    // G303 worker-thread busy-time attribution (worker writes, EE snapshots → relaxed atomics).
    // busyNs is cumulative time the worker spent inside VU1 execute/resume (+ any injected
    // DC2_G303_VU1_SLOW_US spin); the EE-side G146 perf block snapshots deltas per 30-frame window.
    uint64_t workerBusyNs() const { return m_workerBusyNs.load(std::memory_order_relaxed); }
    uint64_t workerKicksRun() const { return m_workerKicksRun.load(std::memory_order_relaxed); }
    uint64_t gsCollectStallNs() const { return m_gsCollectStallNs.load(std::memory_order_relaxed); }
    uint64_t gsCollectStalls() const { return m_gsCollectStalls.load(std::memory_order_relaxed); }
    // G344: TOTAL collectWindowPackets time + call count (all windows). overhead = total - stall.
    uint64_t gsCollectTotalNs() const { return m_gsCollectTotalNs.load(std::memory_order_relaxed); }
    uint64_t gsCollectCalls() const { return m_gsCollectCalls.load(std::memory_order_relaxed); }

    // EE thread, at every drain() window handoff: seal the current kick batch 1:1 with the window
    // (push even when empty, so the worker's per-window pop stays in lockstep).
    void onWindowHandoff()
    {
        const size_t batchKicks = m_curBatch.size();
        {
            std::lock_guard<std::mutex> lk(m_batchMtx);
            m_windowBatches.push_back(std::move(m_curBatch));
            m_curBatch.clear();
        }
        static const bool s_stat = (std::getenv("DC2_G297_STAT") != nullptr);
        if (s_stat)
        {
            ++m_windowsSealed;
            m_kicksInWindows += batchKicks;
            if ((m_windowsSealed % 600u) == 0u)
            {
                size_t pendBatches;
                {
                    std::lock_guard<std::mutex> lk(m_batchMtx);
                    pendBatches = m_windowBatches.size();
                }
                std::fprintf(stderr,
                             "[G297:stat] kicksEnq=%llu windowsSealed=%llu kicksInWindows=%llu "
                             "workerBacklogBatches=%zu\n",
                             (unsigned long long)m_kicksEnq, (unsigned long long)m_windowsSealed,
                             (unsigned long long)m_kicksInWindows, pendBatches);
            }
        }
    }

    // MTGS worker thread, in processWindow: pop this window's batch, wait for its kicks, and append
    // their Path1 packets (kick order) to `out` for merge before the stable path sort.
    void collectWindowPackets(std::vector<std::vector<uint8_t>> &out)
    {
        // G344 premise-gate: time the WHOLE collect call (all windows, all exit paths) so the
        // per-window handoff OVERHEAD = gsCollectTotal - gsCollectStall on a MATCHED all-window
        // denominator (the G316 `merge` timer is 1-in-30 sampled and its variance is VU1-stall,
        // not overhead). RAII covers the early-return (empty-batch / m_stop) paths too.
        const bool g344Instr = g303InstrOn();
        struct G344Timer
        {
            G297Mtvu *self;
            bool on;
            std::chrono::steady_clock::time_point t0;
            ~G344Timer()
            {
                if (!on)
                    return;
                self->m_gsCollectTotalNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count(),
                    std::memory_order_relaxed);
                self->m_gsCollectCalls.fetch_add(1ull, std::memory_order_relaxed);
            }
        } g344Timer{this, g344Instr,
                    g344Instr ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{}};
        std::vector<G297KickPtr> batch;
        {
            std::lock_guard<std::mutex> lk(m_batchMtx);
            if (m_windowBatches.empty())
                return;
            batch = std::move(m_windowBatches.front());
            m_windowBatches.pop_front();
        }
        for (auto &item : batch)
        {
            // Fast path: VU1 worker runs ahead (deep backlog), so this kick is usually already
            // ready — a lock-free atomic load, NO mutex. Only a genuinely-not-yet-produced kick
            // takes the done mutex to block. This is what keeps the GS worker and VU1 worker from
            // convoying on a shared lock (the G189 lesson): the two threads share NO mutex on the
            // hot path (VU1 pop uses m_mtx, done-signal uses m_doneMtx, and this fast path uses
            // neither when ready).
            if (!item->ready.load(std::memory_order_acquire))
            {
                // G303: GS worker stalls here when VU1 has NOT yet produced this kick. Time this
                // wait to attribute the frame's VU1<->GS non-overlap (is the GS worker idle-waiting
                // on VU1, or does VU1 stay ahead?). Gated so the default path adds no clock work.
                const bool instr = g303InstrOn();
                const auto stall0 = instr ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
                {
                    // G482: publish this thread's intent to sleep BEFORE taking the done mutex, so
                    // the VU1 worker's `ready` store and its waiter test cannot both land in the
                    // gap. See the publish site for the full ordering argument.
                    m_doneWaiters.fetch_add(1, std::memory_order_seq_cst);
                    std::unique_lock<std::mutex> lk(m_doneMtx);
                    m_cvDone.wait(lk, [&] { return m_stop || item->ready.load(std::memory_order_acquire); });
                    m_doneWaiters.fetch_sub(1, std::memory_order_seq_cst);
                }
                if (instr)
                {
                    m_gsCollectStallNs.fetch_add(
                        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - stall0).count(),
                        std::memory_order_relaxed);
                    m_gsCollectStalls.fetch_add(1ull, std::memory_order_relaxed);
                }
            }
            if (m_stop)
                return;
            for (auto &pk : item->packets)
                out.push_back(std::move(pk));
        }
    }

    void shutdown()
    {
        if (!m_started)
            return;
        m_stop.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(m_mtx); }      // fence vs m_cvWork waiter
        { std::lock_guard<std::mutex> lk(m_doneMtx); }  // fence vs m_cvDone waiter
        m_cvWork.notify_all();
        m_cvDone.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_started = false;
    }

private:
    static bool envSet(const char *k)
    {
        const char *v = std::getenv(k);
        return v && v[0] && !(v[0] == '0' && v[1] == '\0');
    }

    // G303: cached master gate for the (default-off) worker/GS-stall busy-time attribution. Read once;
    // when off, the promoted default path does zero clock work. On under DC2_PERF (perf-measurement
    // runs) or the standalone DC2_G303_INSTR.
    static bool g303InstrOn()
    {
        static const bool on =
            (std::getenv("DC2_PERF") != nullptr) || (std::getenv("DC2_G303_INSTR") != nullptr);
        return on;
    }

    static bool computeActive()
    {
        // G302: PROMOTED default-on. The VU1 interpreter runs on a dedicated worker thread by
        // default (breaks the EE-VU1 frame pole; MAP-0 -15.3% frame / +18% FPS, EE cpuMs 87->21).
        // Kill switch DC2_G297_NO_MTVU=1 restores the synchronous VU1 path. The old opt-in
        // DC2_G297_MTVU is now a harmless no-op (default is on).
        if (envSet("DC2_G297_NO_MTVU"))
            return false;
        if (!g150_mtgs_enabled())
            return false; // MTVU rides the MTGS window sort/drain
        // VU diagnostics that read shared gs/memory mid-execute, or force the legacy loop, cannot
        // run off-thread safely — fall back to synchronous VU1.
        static const char *const incompat[] = {
            "DC2_G215_BATCH", "DC2_G216_CLIP", "DC2_G216_FORCE_GATE", "DC2_G217_TAG_VU",
            "DC2_G217_JOIN", "DC2_G136_MSCAL", "DC2_TRACE_VU1", "DC2_G295_VU1_DIAG_LOOP",
            "DC2_G138_GSDUMP", "DC2_G217_FORCE_COPY_DRAW"};
        for (const char *e : incompat)
            if (std::getenv(e))
            {
                std::fprintf(stderr, "[G297] MTVU requested but disabled (incompatible diagnostic %s set)\n", e);
                return false;
            }
        std::fprintf(stderr, "[G297] MTVU ACTIVE (G302 default-on; kill DC2_G297_NO_MTVU=1) — VU1 interpreter on a dedicated worker thread\n");
        return true;
    }

    void worker()
    {
        // G478: arm the host PC sampler on THIS thread (no-op unless DC2_G478_VU1PROF=1). The G478
        // premise gate re-opened the VU1 direction (sensitivity 0.80 vs the GS worker's 0.76), and
        // this thread has never had a sample distribution — only occupancy timers.
        g478RegisterVu1WorkerThread();
        // G442 CEILING PROBE (default OFF: DC2_G442_FTZ=1). Nothing in this tree touches MXCSR, so
        // the VU1 interpreter's SSE2 FMACs run with subnormal support ON and every subnormal
        // operand/result costs a microcode assist. Setting FTZ|DAZ on THIS thread only prices that
        // assist: it cannot change any other thread's arithmetic. Output under this flag may be
        // wrong (subnormal lanes read as +0 and the MAC underflow nibble stops being set), so it is
        // a price, not a lever — the workload invariant is kicks/f + pairs/run.
        {
            static const bool s_g442Ftz = (std::getenv("DC2_G442_FTZ") != nullptr);
            if (s_g442Ftz)
            {
                _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
                std::fprintf(stderr, "[G442:ftz] VU1 worker MXCSR=0x%08x (FTZ|DAZ armed)\n",
                             (unsigned)_mm_getcsr());
            }
        }
        for (;;)
        {
            // G441 (diagnostic, DEFAULT OFF: DC2_G441_KICK=1). m_workerBusyNs times only
            // execute()/resume(), so the VU1 worker's per-kick PROLOGUE — the queue pop and the
            // G301 VU-RAM write replay — is invisible to [G303:vu1w] busyMs/f, exactly the class
            // of instrument hole G440 found on the GS worker. Price it before assuming the whole
            // VU1 thread is the pair loop. One cached env read; default path does zero clock work.
            static const bool s_g441Kick = (std::getenv("DC2_G441_KICK") != nullptr);
            const auto g441T0 = s_g441Kick ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            G297KickPtr item;
            // G441 (REJECTED, measured neutral — kept only as this comment): batching this pop
            // into a worker-local prefix queue, so ~1260 lock acquisitions/frame become ~20, is
            // frame-neutral (+0.38 ms, order-balanced blocks disagreeing in sign). [G441:kick]'s
            // popUs of 0.5-4.8 us/kick is the cv/lock ROUND TRIP around an already-full queue, not
            // CPU the pole thread spends — the VU1 worker runs 1000-1800 kicks behind the EE and
            // starves 0.0% of the time. Do not rebuild it.
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                // G441: separate "the queue already had work" (pure lock cost) from STARVATION
                // (the worker had to sleep because the EE has not produced the next kick yet).
                // On the pole thread the second is idle time the frame pays for.
                if (s_g441Kick && m_work.empty())
                {
                    ++m_g441Starved;
                    const auto s0 = std::chrono::steady_clock::now();
                    m_cvWork.wait(lk, [&] { return m_stop || !m_work.empty(); });
                    m_g441StarveNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - s0).count();
                }
                else
                    m_cvWork.wait(lk, [&] { return m_stop || !m_work.empty(); });
                if (m_stop && m_work.empty())
                    return;
                if (s_g441Kick)
                    m_g441Backlog += m_work.size();
                item = m_work.front();
                m_work.pop_front();
            }
            const auto g441T1 = s_g441Kick ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            g_g297KickCapture = &item->packets; // redirect this kick's XGKICKs into item->packets
            g_g297KickTop = item->top;          // captured XTOP (double-buffer top) for this kick
            // G301: replay this kick's ordered VU-RAM writes onto the PERSISTENT worker buffers, in
            // program order, BEFORE running the kick. A previous kick's VU-program writes (SQ/SQI/SQD)
            // already sit in these buffers and persist here — the fix for the G297 per-kick-snapshot
            // data-model drop (pinned in G301). The worker owns these buffers exclusively (the EE never
            // touches them), so no lock is needed.
            {
                size_t run = 0u;
                for (const auto &w : item->writes)
                {
                    uint8_t *dst = w.isCode ? m_workerVuCode : m_workerVuData;
                    const uint32_t cap = w.isCode ? PS2_VU1_CODE_SIZE : PS2_VU1_DATA_SIZE;
                    if (w.off + w.len <= cap && run + w.len <= item->writeBytes.size())
                    {
                        std::memcpy(dst + w.off, item->writeBytes.data() + run, w.len);
                        if (w.isCode)
                        {
                            dc2_g410_vu_code_written(m_workerVuCode, w.off, w.len);
                            // G482: the CODE half of the persistent VU state is tracked as a
                            // monotonic generation instead of a second 16 KB hash. Present in BOTH
                            // A/B arms (one non-atomic worker-local increment), so it cancels.
                            ++g_g482CodeGen;
                        }
                    }
                    run += w.len;
                }
                if (s_g441Kick)
                {
                    m_g441Writes += item->writes.size();
                    m_g441WriteBytes += run;
                }
            }
            if (s_g441Kick)
            {
                const auto g441T2 = std::chrono::steady_clock::now();
                m_g441PopNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g441T1 - g441T0).count();
                m_g441ReplayNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g441T2 - g441T1).count();
            }
            // G303: worker-thread derivative arm — inject a fixed busy-spin per kick to slow ONLY the
            // VU1 worker (the clean "slow VU1 worker" A/B; the old DC2_G295_VU1_DIAG_LOOP arm disables
            // MTVU so it cannot perturb the promoted MTVU-on path). Default 0 (no perturbation).
            static const long long s_g303SlowUs = []() {
                const char *v = std::getenv("DC2_G303_VU1_SLOW_US");
                return v ? std::atoll(v) : 0LL;
            }();
            // ---- G482 (BOTH halves default-off) ----------------------------------------------
            // (a) PRICE arm `DC2_G419_AB=memohash`: compute the full memo KEY and sink it. No clock
            //     read, no map, no branch on the result — the candidate arm pays exactly the hash
            //     and nothing else, so the paired window difference IS the key's frame cost.
            // (b) CENSUS `DC2_G482_MEMOCENSUS=1`: the same key, plus a post-state key, an exec
            //     timer and an intra-frame table, to read the TRUE full-identity hit rate and the
            //     execute time those hits carry. Perturbs the pole (three extra clock reads and a
            //     second 16 KB hash per kick) and is therefore never a timing arm.
            const bool g482Census = g482CensusOn();
            const int g482Arm = g482MemoHashArm();
            uint64_t g482PreKey = 0ull, g482PreNs = 0ull;
            uint64_t g482DataK = 0ull, g482RegK = 0ull, g482ParamK = 0ull;
            std::chrono::steady_clock::time_point g482Exec0;
            if (g482Census)
            {
                g482FrameTick();
                ++g482Win().kicks;
                if (item->isResume)
                    ++g482Win().resume;
            }
            if (!item->isResume && (g482Census || g482Arm == 1))
            {
                if (g482Census)
                {
                    const auto h0 = std::chrono::steady_clock::now();
                    g482PreKey = g482StateKey(m_workerVuData, PS2_VU1_DATA_SIZE, m_vu1->state(),
                                              item->startPC, item->itop, item->top, item->budget);
                    g482PreNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - h0).count();
                    // Split keys (census only, OUTSIDE the timed region) so a miss can be
                    // attributed to the data RAM or to the registers.
                    g482DataK = g482DataKey(m_workerVuData, PS2_VU1_DATA_SIZE);
                    g482RegK = g482RegKey(m_vu1->state());
                    g482ParamK = g482ParamKey(item->startPC, item->itop, item->top,
                                              item->writeBytes);
                }
                else
                    g_g482HashSink ^= g482StateKey(m_workerVuData, PS2_VU1_DATA_SIZE,
                                                   m_vu1->state(), item->startPC, item->itop,
                                                   item->top, item->budget);
            }
            if (g482Census)
                g482Exec0 = std::chrono::steady_clock::now();
            // Busy-time attribution is gated so the default (non-measuring) path does zero clock work.
            const bool instr = g303InstrOn();
            const auto twBusy0 = instr ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
            if (item->isResume)
                m_vu1->resume(m_workerVuCode, PS2_VU1_CODE_SIZE, m_workerVuData, PS2_VU1_DATA_SIZE,
                              *m_gs, m_mem, item->itop, item->budget);
            else
                m_vu1->execute(m_workerVuCode, PS2_VU1_CODE_SIZE, m_workerVuData, PS2_VU1_DATA_SIZE,
                               *m_gs, m_mem, item->startPC, item->itop, item->budget);
            // G438: when DC2_G419_AB=vu1slow is armed the spin runs on CANDIDATE frames only, so the
            // probe becomes a within-process paired measurement instead of a difference of two run
            // means (whose run-level spread is ~4.5 ms here — larger than the effect). The spin
            // length still comes from DC2_G303_VU1_SLOW_US, defaulting to 5 us/kick.
            {
                const int g438Arm = g438SlowArm(0);
                const long long spinUs = s_g303SlowUs > 0 ? s_g303SlowUs : 5LL;
                const bool doSpin = (g438Arm >= 0) ? (g438Arm == 1) : (s_g303SlowUs > 0);
                if (doSpin)
                {
                    const auto spinUntil = std::chrono::steady_clock::now() +
                                           std::chrono::microseconds(spinUs);
                    while (std::chrono::steady_clock::now() < spinUntil) { /* busy-spin, worker only */ }
                }
            }
            if (instr)
            {
                m_workerBusyNs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - twBusy0).count(),
                    std::memory_order_relaxed);
                m_workerKicksRun.fetch_add(1ull, std::memory_order_relaxed);
            }
            g_g297KickCapture = nullptr;
            // G482 census tail: post-state key + intra-frame table. `item->packets` is final here,
            // so its byte count is exactly what a memo hit would have to replay.
            if (g482Census && !item->isResume)
            {
                const auto e1 = std::chrono::steady_clock::now();
                const uint64_t execNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    e1 - g482Exec0).count();
                const uint64_t postKey = g482StateKey(m_workerVuData, PS2_VU1_DATA_SIZE,
                                                      m_vu1->state(), 0u, 0u, 0u, 0u);
                uint64_t pkt = 0ull;
                for (const auto &pk : item->packets)
                    pkt += pk.size();
                G482Win &wa = g482Win();
                wa.hashNs += g482PreNs; // PRE key only — the post key is census-only overhead
                ++wa.hashCalls;
                wa.execNsAll += execNs;
                auto wit = g482WeakMap().find(g482ParamK);
                if (wit == g482WeakMap().end())
                    g482WeakMap().emplace(g482ParamK, G482WeakRec{g482DataK, g482RegK});
                else
                {
                    ++wa.weakDup;
                    if (wit->second.dataKey == g482DataK)
                        ++wa.weakDataMatch;
                    if (wit->second.regKey == g482RegK)
                        ++wa.weakRegMatch;
                }
                auto it = g482FrameMap().find(g482PreKey);
                if (it == g482FrameMap().end())
                    g482FrameMap().emplace(g482PreKey, G482PostRec{postKey, execNs, pkt, 1u});
                else
                {
                    ++it->second.count;
                    ++wa.dupIntra;
                    if (it->second.postKey != postKey)
                        ++wa.dupUnsound; // key insufficient => memo would be WRONG
                    wa.execNsDup += execNs;
                    wa.dupPktBytes += pkt;
                    wa.dupStateBytes += PS2_VU1_DATA_SIZE;
                }
            }
            const auto g441T3 = s_g441Kick ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            {
                // G482: SKIP THE PUBLISH LOCK AND NOTIFY WHEN NOBODY IS WAITING.
                //
                // The VU1 worker runs ~1,900 kicks AHEAD of the GS worker ([G441:kick]
                // backlog=1916, starve=0.0%), so collectWindowPackets takes its fast lock-free
                // `ready` path on essentially every kick and NOTHING ever sleeps on m_cvDone. The
                // old code still paid the `m_doneMtx` acquire/release plus a `notify_all` syscall
                // path on all ~1,236 kicks/frame (~0.25 us/kick, ~0.31 ms/f) on the SOLE POLE.
                //
                // ORDERING (no lost wakeup). A waiter does `m_doneWaiters++` (seq_cst) BEFORE
                // taking m_doneMtx and re-checking `ready`. The publisher does `ready.store`
                // (seq_cst) BEFORE `m_doneWaiters.load` (seq_cst). Both pairs are in the single
                // total order, so for any waiter/publisher pair either:
                //   - the increment precedes the load  -> the publisher sees a non-zero count and
                //     takes the lock + notifies, or
                //   - the load precedes the increment  -> the store precedes the increment too,
                //     so the waiter's own predicate re-check (under the lock, after the increment)
                //     already observes ready == true and never blocks.
                // Rollback: DC2_G482_NO_NOTIFYSKIP=1   Paired gate: DC2_G419_AB=notifyskip
                const int g482Arm2 = g482NotifySkipArm();
                if (g482Arm2 == 1 || (g482Arm2 < 0 && g482NotifySkipOn()))
                {
                    item->ready.store(true, std::memory_order_seq_cst);
                    if (m_doneWaiters.load(std::memory_order_seq_cst) != 0)
                    {
                        { std::lock_guard<std::mutex> lk(m_doneMtx); }
                        m_cvDone.notify_all();
                    }
                }
                else
                {
                    // Publish ready under the DONE mutex (separate from the work-queue m_mtx, so
                    // this does not contend with the GS worker's batch collection or the EE's
                    // enqueue). The lock closes the lost-wakeup gap against collectWindowPackets'
                    // slow-path wait.
                    {
                        std::lock_guard<std::mutex> lk(m_doneMtx);
                        item->ready.store(true, std::memory_order_release);
                    }
                    m_cvDone.notify_all();
                }
            }
            if (s_g441Kick)
            {
                const auto g441T4 = std::chrono::steady_clock::now();
                m_g441PublishNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g441T4 - g441T3).count();
                const uint64_t g441ExecNs =
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g441T3 - g441T0).count() -
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        g441T1 - g441T0).count();
                m_g441ExecNs += g441ExecNs;
                // G483: split the exec bucket by KICK KIND and record the pair-loop iteration
                // count, so ns/pair is directly comparable between the two populations. G482
                // measured execute()/resume() at 98.4% of the kick span as ONE number; every
                // census this arc (G331/G479/G482) then excluded resume kicks, so ~30% of the
                // pole's work has never been characterised. `cycle` is the pair loop's own
                // counter, published by vu1_g483_run_cycles.inc.
                {
                    const uint64_t pairs = dc2_g483_last_run_cycles();
                    const uint32_t stop = dc2_g483_last_stop_reason();
                    m_g483Pairs += pairs;
                    if (stop == 0u)
                    {
                        ++m_g483MaxCycKicks;
                        m_g483MaxCycPairs += pairs;
                    }
                    if (item->isResume)
                    {
                        ++m_g483ResKicks;
                        m_g483ResExecNs += g441ExecNs;
                        m_g483ResPairs += pairs;
                        if (stop == 0u)
                            ++m_g483ResMaxCyc;
                    }
                }
                m_g441KickNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g441T4 - g441T0).count();
                if ((++m_g441Kicks % 30000ull) == 0ull)
                {
                    // Per-kick microseconds; multiply by kicks/f (~1260 on lean MAP-0) for ms/f.
                    const double n = (double)(m_g441Kicks - m_g441LastKicks);
                    std::fprintf(stderr,
                                 "[G441:kick] kicks=%llu | kickUs=%.2f popUs=%.2f replayUs=%.2f "
                                 "execUs=%.2f publishUs=%.2f | starve=%.1f%%/%.2fus backlog=%.1f "
                                 "| writes/kick=%.1f bytes/kick=%.0f\n",
                                 (unsigned long long)m_g441Kicks,
                                 (double)(m_g441KickNs - m_g441LastKickNs) / 1e3 / n,
                                 (double)(m_g441PopNs - m_g441LastPopNs) / 1e3 / n,
                                 (double)(m_g441ReplayNs - m_g441LastReplayNs) / 1e3 / n,
                                 (double)(m_g441ExecNs - m_g441LastExecNs) / 1e3 / n,
                                 (double)(m_g441PublishNs - m_g441LastPublishNs) / 1e3 / n,
                                 100.0 * (double)(m_g441Starved - m_g441LastStarved) / n,
                                 (double)(m_g441StarveNs - m_g441LastStarveNs) / 1e3 / n,
                                 (double)(m_g441Backlog - m_g441LastBacklog) / n,
                                 (double)(m_g441Writes - m_g441LastWrites) / n,
                                 (double)(m_g441WriteBytes - m_g441LastWriteBytes) / n);
                    // G483: kick-kind split of the same window. `resExecUs`/`geoExecUs` are per
                    // kick OF THAT KIND (not per kick overall), so they compare directly; the
                    // ns/pair columns say whether a kind is expensive because it runs more pairs
                    // or because its pairs are slower.
                    {
                        const double rk = (double)(m_g483ResKicks - m_g483LastResKicks);
                        const double gk = n - rk;
                        const double rp = (double)(m_g483ResPairs - m_g483LastResPairs);
                        const double gp = (double)(m_g483Pairs - m_g483LastPairs) - rp;
                        const double rns = (double)(m_g483ResExecNs - m_g483LastResExecNs);
                        const double gns = (double)(m_g441ExecNs - m_g441LastExecNs) - rns;
                        std::fprintf(stderr,
                                     "[G483:kind] resume=%.1f%% | res(kicks=%.0f execUs=%.2f "
                                     "pairs=%.0f nsPerPair=%.1f) geo(kicks=%.0f execUs=%.2f "
                                     "pairs=%.0f nsPerPair=%.1f) | resShareOfExec=%.1f%% "
                                     "resShareOfPairs=%.1f%%\n",
                                     100.0 * rk / n,
                                     rk, rk > 0.0 ? rns / 1e3 / rk : 0.0,
                                     rk > 0.0 ? rp / rk : 0.0, rp > 0.0 ? rns / rp : 0.0,
                                     gk, gk > 0.0 ? gns / 1e3 / gk : 0.0,
                                     gk > 0.0 ? gp / gk : 0.0, gp > 0.0 ? gns / gp : 0.0,
                                     100.0 * rns / (rns + gns > 0.0 ? rns + gns : 1.0),
                                     100.0 * rp / (rp + gp > 0.0 ? rp + gp : 1.0));
                        // Budget exhaustion: a kick whose pair loop ended because `maxCycles` ran
                        // out, not because the microprogram latched its E bit. Split by kind, and
                        // report the share of ALL pairs those kicks account for.
                        const double mk = (double)(m_g483MaxCycKicks - m_g483LastMaxCycKicks);
                        const double mp = (double)(m_g483MaxCycPairs - m_g483LastMaxCycPairs);
                        const double rmk = (double)(m_g483ResMaxCyc - m_g483LastResMaxCyc);
                        std::fprintf(stderr,
                                     "[G483:stop] maxcycKicks=%.1f%% (res=%.1f%% geo=%.1f%%) "
                                     "maxcycPairs=%.1f%% ofAll pairs/maxcycKick=%.0f\n",
                                     100.0 * mk / n,
                                     rk > 0.0 ? 100.0 * rmk / rk : 0.0,
                                     gk > 0.0 ? 100.0 * (mk - rmk) / gk : 0.0,
                                     (rp + gp) > 0.0 ? 100.0 * mp / (rp + gp) : 0.0,
                                     mk > 0.0 ? mp / mk : 0.0);
                        m_g483LastMaxCycKicks = m_g483MaxCycKicks;
                        m_g483LastMaxCycPairs = m_g483MaxCycPairs;
                        m_g483LastResMaxCyc = m_g483ResMaxCyc;
                        m_g483LastResKicks = m_g483ResKicks;
                        m_g483LastResExecNs = m_g483ResExecNs;
                        m_g483LastResPairs = m_g483ResPairs;
                        m_g483LastPairs = m_g483Pairs;
                    }
                    m_g441LastKicks = m_g441Kicks;
                    m_g441LastKickNs = m_g441KickNs;
                    m_g441LastPopNs = m_g441PopNs;
                    m_g441LastReplayNs = m_g441ReplayNs;
                    m_g441LastExecNs = m_g441ExecNs;
                    m_g441LastPublishNs = m_g441PublishNs;
                    m_g441LastStarved = m_g441Starved;
                    m_g441LastStarveNs = m_g441StarveNs;
                    m_g441LastBacklog = m_g441Backlog;
                    m_g441LastWrites = m_g441Writes;
                    m_g441LastWriteBytes = m_g441WriteBytes;
                }
            }
        }
    }

    // G441 per-kick prologue accounting (VU1 worker thread ONLY — plain scalars, no atomics,
    // and every one of them is written only under DC2_G441_KICK=1).
    uint64_t m_g441Kicks = 0ull, m_g441KickNs = 0ull, m_g441PopNs = 0ull, m_g441ReplayNs = 0ull;
    uint64_t m_g441ExecNs = 0ull, m_g441PublishNs = 0ull;
    uint64_t m_g441Writes = 0ull, m_g441WriteBytes = 0ull;
    uint64_t m_g441LastKicks = 0ull, m_g441LastKickNs = 0ull, m_g441LastPopNs = 0ull;
    uint64_t m_g441LastReplayNs = 0ull, m_g441LastExecNs = 0ull, m_g441LastPublishNs = 0ull;
    uint64_t m_g441LastWrites = 0ull, m_g441LastWriteBytes = 0ull;
    uint64_t m_g441Starved = 0ull, m_g441StarveNs = 0ull, m_g441Backlog = 0ull;
    uint64_t m_g441LastStarved = 0ull, m_g441LastStarveNs = 0ull, m_g441LastBacklog = 0ull;
    // G483 kick-kind split (same DC2_G441_KICK=1 gate, same thread, same plain-scalar rules).
    uint64_t m_g483ResKicks = 0ull, m_g483ResExecNs = 0ull, m_g483ResPairs = 0ull;
    uint64_t m_g483Pairs = 0ull;
    uint64_t m_g483LastResKicks = 0ull, m_g483LastResExecNs = 0ull, m_g483LastResPairs = 0ull;
    uint64_t m_g483LastPairs = 0ull;
    uint64_t m_g483MaxCycKicks = 0ull, m_g483MaxCycPairs = 0ull, m_g483ResMaxCyc = 0ull;
    uint64_t m_g483LastMaxCycKicks = 0ull, m_g483LastMaxCycPairs = 0ull, m_g483LastResMaxCyc = 0ull;

    VU1Interpreter *m_vu1 = nullptr;
    PS2Memory *m_mem = nullptr;
    GS *m_gs = nullptr;
    std::thread m_thread;
    bool m_started = false;
    std::atomic<bool> m_stop{false}; // read under either mutex's cv predicate → atomic
    std::mutex m_mtx;                                   // work-queue: EE enqueue / VU1 worker pop
    std::mutex m_doneMtx;                               // done-signal: VU1 worker / GS worker (decoupled)
    std::condition_variable m_cvWork;                  // paired with m_mtx
    std::condition_variable m_cvDone;                  // paired with m_doneMtx
    // G482: number of threads that have declared intent to sleep on m_cvDone but have not yet
    // returned from wait(). Incremented BEFORE m_doneMtx is taken (see the publish site's ordering
    // argument). Zero on essentially every kick, which is what makes the publish lock skippable.
    std::atomic<int> m_doneWaiters{0};
    std::deque<G297KickPtr> m_work;                     // worker input FIFO
    std::vector<G297KickPtr> m_curBatch;               // EE-only: kicks in the current window
    std::mutex m_batchMtx;
    std::deque<std::vector<G297KickPtr>> m_windowBatches; // per-window batches (EE push / worker pop)
    // G301: worker-owned PERSISTENT VU RAM (worker thread ONLY — no lock). Zero = VU reset state;
    // ALL content arrives via replayed UNPACK/MPG writes. VIF1 is the complete funnel into VU RAM
    // (verified: no memory-mapped guest Store path writes m_vu1Data/m_vu1Code in this runtime).
    uint8_t m_workerVuCode[PS2_VU1_CODE_SIZE] = {};
    uint8_t m_workerVuData[PS2_VU1_DATA_SIZE] = {};
    // G301: EE-local accumulator of VU-RAM writes for the current (not-yet-enqueued) kick.
    std::vector<G297VuWrite> m_pendWrites;
    std::vector<uint8_t> m_pendWriteBytes;
    // Diagnostics (DC2_G297_STAT), EE-thread only:
    unsigned long long m_kicksEnq = 0ull;
    unsigned long long m_windowsSealed = 0ull;
    unsigned long long m_kicksInWindows = 0ull;
    // G303 worker-thread busy-time attribution (worker writes, EE snapshots).
    std::atomic<uint64_t> m_workerBusyNs{0ull};
    std::atomic<uint64_t> m_workerKicksRun{0ull};
    // G303 GS-worker stall on VU1 (GS-worker-written in collectWindowPackets, EE-snapshot).
    std::atomic<uint64_t> m_gsCollectStallNs{0ull};
    std::atomic<uint64_t> m_gsCollectStalls{0ull};
    // G344 premise-gate: total collectWindowPackets time + call count (overhead = total - stall).
    std::atomic<uint64_t> m_gsCollectTotalNs{0ull};
    std::atomic<uint64_t> m_gsCollectCalls{0ull};
};

} // namespace

// Free bridges consumed by ps2_gif_arbiter.cpp (declared extern there).
bool g297MtvuActive() { return G297Mtvu::activeCached(); }
void g297NoteVuWrite(bool isCode, uint32_t off, const uint8_t *src, uint32_t bytes)
{
    if (G297Mtvu::activeCached())
        G297Mtvu::instance().noteVuWrite(isCode, off, src, bytes);
}
bool g297HasPendingKicks()
{
    return G297Mtvu::activeCached() && G297Mtvu::instance().hasPendingKicks();
}
void g297OnWindowHandoff()
{
    if (G297Mtvu::activeCached())
        G297Mtvu::instance().onWindowHandoff();
}
void g297CollectWindowPackets(std::vector<std::vector<uint8_t>> &out)
{
    if (G297Mtvu::activeCached())
        G297Mtvu::instance().collectWindowPackets(out);
}
void g297ShutdownMtvu()
{
    G297Mtvu::instance().shutdown();
}
// G303 worker-thread busy-time attribution bridges (0 when MTVU inactive).
uint64_t g297WorkerBusyNs() { return G297Mtvu::activeCached() ? G297Mtvu::instance().workerBusyNs() : 0ull; }
uint64_t g297WorkerKicksRun() { return G297Mtvu::activeCached() ? G297Mtvu::instance().workerKicksRun() : 0ull; }
uint64_t g297GsCollectStallNs() { return G297Mtvu::activeCached() ? G297Mtvu::instance().gsCollectStallNs() : 0ull; }
uint64_t g297GsCollectStalls() { return G297Mtvu::activeCached() ? G297Mtvu::instance().gsCollectStalls() : 0ull; }
uint64_t g297GsCollectTotalNs() { return G297Mtvu::activeCached() ? G297Mtvu::instance().gsCollectTotalNs() : 0ull; }
uint64_t g297GsCollectCalls() { return G297Mtvu::activeCached() ? G297Mtvu::instance().gsCollectCalls() : 0ull; }


// ============================================================================
// F60 spin diagnostics (global linkage; referenced by extern in the primitive
// TUs). A guest EE busy-wait that never returns hammers exactly one of these
// poll/wait primitives. The host present loop stays alive during the stall and
// samples per-interval deltas (see "[F60:spin]" below) so we can identify the
// spinning primitive and the guest return address it is polling from.
// All quiet/no-op unless DC2_TRACE_F60 is set.
// ============================================================================
std::atomic<uint64_t> g_f60_checkStatRpc{0};
std::atomic<uint64_t> g_f60_pollSema{0};
std::atomic<uint64_t> g_f60_dmaStat{0};
std::atomic<uint64_t> g_f60_gsSyncV{0};
std::atomic<uint64_t> g_f60_waitVSyncTick{0};
std::atomic<uint64_t> g_f60_waitSemaBlocked{0};
std::atomic<uint32_t> g_f60_checkStatRpc_ra{0};
std::atomic<uint32_t> g_f60_checkStatRpc_client{0};
std::atomic<uint32_t> g_f60_pollSema_ra{0};
std::atomic<uint32_t> g_f60_waitSema_ra{0};
std::atomic<int32_t> g_f60_waitSema_sid{0};

// ============================================================================
// Syscall-dispatch path audit (env-gated DC2_TRACE_SYSCALL_PATH; quiet by
// default). Counts how each EE syscall resolves: encoded immediate, $v1 (the
// canonical EE number reg), or nothing (→ [Syscall TODO]). F64.5 removed the
// legacy $v0 fallback after this probe measured 0 hits for DC2; anything that
// now lands in [Syscall TODO] is a real missing/mis-emitted syscall to fix.
// ============================================================================
std::atomic<uint64_t> g_scpath_v1{0};   // resolved via $v1 (canonical)
std::atomic<uint64_t> g_scpath_enc{0};  // resolved via encoded immediate
std::atomic<uint64_t> g_scpath_todo{0}; // resolved by nothing (→ [Syscall TODO])

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif

static bool runtimeEnvFlagEnabled(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr &&
           value[0] != '\0' &&
           std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

static uint32_t dc2Vu1CycleBudget()
{
    static const uint32_t budget = []()
    {
        const char *value = std::getenv("DC2_G22_VU1_CYCLES");
        if (!value || value[0] == '\0')
            return 65536u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value || parsed == 0ul)
            return 65536u;
        if (parsed > 1048576ul)
            return 1048576u;
        return static_cast<uint32_t>(parsed);
    }();
    return budget;
}
