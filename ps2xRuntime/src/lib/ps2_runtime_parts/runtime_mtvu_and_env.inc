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
        m_pendWrites.push_back(G297VuWrite{off, bytes, static_cast<uint8_t>(isCode ? 1u : 0u)});
        m_pendWriteBytes.insert(m_pendWriteBytes.end(), src, src + bytes);
    }

    // EE thread: attach the VU-RAM writes accumulated since the last kick, enqueue to the worker,
    // return immediately. The worker owns/persists VU RAM, so there is no per-kick full snapshot.
    void enqueueKick(uint32_t startPC, uint32_t itop, uint32_t top, uint32_t budget, bool isResume)
    {
        auto item = std::make_shared<G297KickItem>();
        item->writes = std::move(m_pendWrites);        // EE-local until pushed: move, never copy
        item->writeBytes = std::move(m_pendWriteBytes);
        m_pendWrites.clear();                          // moved-from vectors → empty + reusable
        m_pendWriteBytes.clear();
        item->startPC = startPC;
        item->itop = itop;
        item->top = top;
        item->budget = budget;
        item->isResume = isResume;
        m_curBatch.push_back(item);                    // EE-only accumulator (issue order)
        ++m_kicksEnq;
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
            std::unique_lock<std::mutex> lk(m_doneMtx);
            m_cvDone.wait(lk, [&] { return m_stop || item->ready.load(std::memory_order_acquire); });
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
                    std::unique_lock<std::mutex> lk(m_doneMtx);
                    m_cvDone.wait(lk, [&] { return m_stop || item->ready.load(std::memory_order_acquire); });
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
        for (;;)
        {
            G297KickPtr item;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [&] { return m_stop || !m_work.empty(); });
                if (m_stop && m_work.empty())
                    return;
                item = m_work.front();
                m_work.pop_front();
            }
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
                        std::memcpy(dst + w.off, item->writeBytes.data() + run, w.len);
                    run += w.len;
                }
            }
            // G303: worker-thread derivative arm — inject a fixed busy-spin per kick to slow ONLY the
            // VU1 worker (the clean "slow VU1 worker" A/B; the old DC2_G295_VU1_DIAG_LOOP arm disables
            // MTVU so it cannot perturb the promoted MTVU-on path). Default 0 (no perturbation).
            static const long long s_g303SlowUs = []() {
                const char *v = std::getenv("DC2_G303_VU1_SLOW_US");
                return v ? std::atoll(v) : 0LL;
            }();
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
            if (s_g303SlowUs > 0)
            {
                const auto spinUntil = std::chrono::steady_clock::now() +
                                       std::chrono::microseconds(s_g303SlowUs);
                while (std::chrono::steady_clock::now() < spinUntil) { /* busy-spin, worker only */ }
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
            {
                // Publish ready under the DONE mutex (separate from the work-queue m_mtx, so this
                // does not contend with the GS worker's batch collection or the EE's enqueue). The
                // lock closes the lost-wakeup gap against collectWindowPackets' slow-path wait.
                std::lock_guard<std::mutex> lk(m_doneMtx);
                item->ready.store(true, std::memory_order_release);
            }
            m_cvDone.notify_all();
        }
    }

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

