// ============================================================================
// G480 — cross-thread Path1 packet-buffer RECYCLING (the VU1 allocator residue)
//
// WHY THIS EXISTS. G478's first PC sample of the MTVU worker found it 82.75% inside
// VU1Interpreter::run with ~4% spread over RtlAllocateHeap / malloc / operator new /
// _Uninitialized_move / _Destroy_range. G478 built a pool for the per-kick G297KickItem and
// measured it NEUTRAL (+0.065 ms/f, t = 0.76) — and root-caused its own failure: the ~4% does not
// belong to the item, it belongs to the per-XGKICK Path1 packet buffers. Each XGKICK does
//
//     g_g297KickCapture->emplace_back(pkt, pkt + bytes);   // one fresh malloc, VU1 worker
//
// and that buffer's OWNERSHIP TRANSFERS: collectWindowPackets MOVES it into the GS worker's window
// list, the GS worker moves it again into a GifArbiterPacket, and the FREE happens on the GS
// worker when the window's packet vector dies. A VU1-side free list therefore never sees the
// buffer again (G478's durable lesson: "a pool cannot recycle a buffer whose ownership transfers —
// the fix is an ownership change, not a pool").
//
// THE OWNERSHIP CHANGE. This file builds the missing RETURN PATH:
//
//     VU1 worker              shared bin (mutex)              GS worker
//     ----------              ------------------              ---------
//     acquire from a          <-- refill batch of 64 --
//     thread_local free                                       stage each consumed window
//     list; assign() the                                      buffer into a thread_local
//     packet bytes into it     -- return batch, 1 lock/window ->  list; flush once per window
//
// The hot side (VU1) touches NO lock while its local list is non-empty, so the per-packet cost
// falls to a pop_back + `assign` into memory that already has capacity. The cold side (GS worker,
// sensitivity 0.08 today) pays one mutex per window instead of ~n frees per window.
//
// EXACTNESS. A recycled buffer is `assign(src, src + bytes)`-ed, so its `data()`/`size()` are
// bit-identical to `emplace_back(src, src + bytes)`. Only CAPACITY survives a recycle, and no
// consumer of a Path1 packet can observe capacity (every reader takes `data()` + `size()`). The
// control arm of the paired gate is the exact pre-G480 `emplace_back` statement.
//
// BOUNDS. Only buffers whose capacity is in (0, kG480MaxBufCap] are retained, so a Path3 IMAGE
// upload (which can be megabytes) is never parked in the bin; the bin itself is hard-capped at
// kG480MaxBinCap buffers. The bin is a deliberately leaked process-lifetime static, so a buffer
// still in flight at shutdown always has a valid home (same reasoning as G478KickPool).
//
// Rollback: DC2_G480_NO_PKTPOOL=1. Paired gate: DC2_G419_AB=pktpool. Census: DC2_G480_PKTCENSUS=1.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// File scope on purpose: inside an anonymous namespace this would get INTERNAL linkage and fail to
// resolve against ps2_runtime.cpp's definition (the G214 trap). Re-declaring an already-declared
// extern with the same type at the same scope is well-formed.
extern std::atomic<uint64_t> g_dc2PresentTick;

struct G480PktBin
{
    std::mutex mtx;
    std::vector<std::vector<uint8_t>> free;
};

// Defined exactly once, in ps2_gif_arbiter.cpp (the TU that owns the return side).
G480PktBin &g480PktBin();
// G419 paired-gate arm: -1 = this lever is not selected (honour the rollback flag), 0 = control
// frame (the exact pre-G480 fresh allocation), 1 = candidate frame (recycled buffer).
extern int g480PktPoolArm();

// Bounds sized from the census (DC2_G480_PKTCENSUS=1, lean MAP-0): 1,950 Path1 packets/frame at
// 420 bytes each, 100% hit at 30 refills/frame. A bin of 4,096 is ~2 frames of supply, and the 8 KB
// capacity filter keeps a Path3 IMAGE upload (or an unusually large packet) out of the bin, so the
// retained set is bounded at ~32 MB worst case and ~2 MB in practice.
static constexpr size_t kG480MaxBufCap = 8u * 1024u; // never park an IMAGE-sized buffer
static constexpr size_t kG480MaxBinCap = 4096u;      // hard bound on retained buffers
static constexpr size_t kG480RefillN = 64u;          // buffers per VU1-side refill (1 lock per 64)

// Default-ON once promoted. File-scope const, not a function-local static: this guard sits in a
// per-XGKICK path on the pole thread, where even a magic-static guard check is measurable (G268).
static const bool s_g480PktPoolOn = (std::getenv("DC2_G480_NO_PKTPOOL") == nullptr);
inline bool g480PktPoolOn() { return s_g480PktPoolOn; }

// Is `pktpool` the SELECTED A/B lever? Resolved once at static-init so the default path never makes
// the cross-TU g480PktPoolArm() call at all (G424: a cross-TU call in a per-item path is real cost).
static const bool s_g480AbSelected = [] {
    const char *v = std::getenv("DC2_G419_AB");
    return v != nullptr && std::strcmp(v, "pktpool") == 0;
}();

// ---------------------------------------------------------------------------
// Census (default OFF: DC2_G480_PKTCENSUS=1). Prices the site's CALL RATE before anything is
// concluded about it (G476 rule), and reports the pool hit rate so a miss-dominated pool can never
// be mistaken for a neutral mechanism. VU1-worker-only plain scalars — no atomics on the hot path.
struct G480PktCensus
{
    uint64_t captures = 0ull, bytes = 0ull, hits = 0ull, misses = 0ull, refills = 0ull;
    uint64_t lastTick = 0ull, lastCaptures = 0ull, lastBytes = 0ull, lastHits = 0ull;
    uint64_t frames = 0ull;
};

static const bool s_g480CensusOn = (std::getenv("DC2_G480_PKTCENSUS") != nullptr);
inline bool g480PktCensusOn() { return s_g480CensusOn; }

// ---------------------------------------------------------------------------
// VU1 WORKER: capture one XGKICK Path1 packet into this kick's private list.
// `out.emplace_back(src, src + bytes)` is the exact pre-G480 statement and remains the control arm.
inline void g480CapturePacket(std::vector<std::vector<uint8_t>> &out,
                              const uint8_t *src, uint32_t bytes)
{
    const bool pooled = s_g480AbSelected ? (g480PktPoolArm() == 1) : s_g480PktPoolOn;

    static thread_local std::vector<std::vector<uint8_t>> s_local;
    static thread_local G480PktCensus s_census;
    const bool census = s_g480CensusOn;

    bool hit = false;
    if (pooled)
    {
        if (s_local.empty())
        {
            G480PktBin &bin = g480PktBin();
            std::lock_guard<std::mutex> lk(bin.mtx);
            size_t n = bin.free.size();
            if (n > kG480RefillN)
                n = kG480RefillN;
            for (size_t i = 0u; i < n; ++i)
            {
                s_local.push_back(std::move(bin.free.back()));
                bin.free.pop_back();
            }
            if (census)
                ++s_census.refills;
        }
        if (!s_local.empty())
        {
            std::vector<uint8_t> buf(std::move(s_local.back()));
            s_local.pop_back();
            buf.assign(src, src + bytes); // no allocation while capacity suffices
            out.push_back(std::move(buf));
            hit = true;
        }
    }
    if (!hit)
        out.emplace_back(src, src + bytes); // exact pre-G480 form (control arm / pool miss)

    if (census)
    {
        ++s_census.captures;
        s_census.bytes += bytes;
        if (hit)
            ++s_census.hits;
        else
            ++s_census.misses;
        const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
        if (tick != s_census.lastTick)
        {
            s_census.lastTick = tick;
            if (++s_census.frames >= 120ull)
            {
                const double f = (double)s_census.frames;
                const uint64_t dCap = s_census.captures - s_census.lastCaptures;
                const uint64_t dBytes = s_census.bytes - s_census.lastBytes;
                const uint64_t dHits = s_census.hits - s_census.lastHits;
                size_t binNow = 0u;
                {
                    G480PktBin &bin = g480PktBin();
                    std::lock_guard<std::mutex> lk(bin.mtx);
                    binNow = bin.free.size();
                }
                std::fprintf(stderr,
                             "[G480:pkt] frames=%llu pkts/f=%.0f bytes/pkt=%.0f hit=%.1f%% "
                             "refills/f=%.1f binNow=%zu\n",
                             (unsigned long long)s_census.frames, (double)dCap / f,
                             dCap ? (double)dBytes / (double)dCap : 0.0,
                             dCap ? 100.0 * (double)dHits / (double)dCap : 0.0,
                             (double)s_census.refills / f, binNow);
                s_census.frames = 0ull;
                s_census.refills = 0ull;
                s_census.lastCaptures = s_census.captures;
                s_census.lastBytes = s_census.bytes;
                s_census.lastHits = s_census.hits;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// GS WORKER: stage each consumed window buffer, then flush the whole window in ONE lock.
// Staging is called only after the window's dispatch loop has finished with every packet, so the
// moved-from GifArbiterPacket::data is never read again (processWindow returns immediately after).
inline std::vector<std::vector<uint8_t>> &g480StagedReturns()
{
    static thread_local std::vector<std::vector<uint8_t>> s_staged;
    return s_staged;
}

inline void g480StageReturn(std::vector<uint8_t> &&buf)
{
    const size_t cap = buf.capacity();
    if (cap == 0u || cap > kG480MaxBufCap)
        return; // let it die normally (already empty, or an IMAGE-sized upload buffer)
    g480StagedReturns().push_back(std::move(buf));
}

// One mutex acquisition per WINDOW (~1,560/f on the GS worker, sensitivity 0.08) instead of one
// free per packet on the VU1 worker (the pole). Buffers past the bin cap are simply destroyed.
inline void g480FlushReturns()
{
    std::vector<std::vector<uint8_t>> &staged = g480StagedReturns();
    if (staged.empty())
        return;
    {
        G480PktBin &bin = g480PktBin();
        std::lock_guard<std::mutex> lk(bin.mtx);
        for (auto &b : staged)
        {
            if (bin.free.size() >= kG480MaxBinCap)
                break;
            bin.free.push_back(std::move(b));
        }
    }
    staged.clear();
}

// GS WORKER, end of processWindow: recycle every packet buffer this window consumed. Gated on the
// SAME arm as the capture side so a control frame pays neither half of the mechanism (a return-only
// control frame would charge the GS worker for work whose benefit lands on VU1, biasing the gate).
inline bool g480RecycleOn()
{
    return s_g480AbSelected ? (g480PktPoolArm() == 1) : s_g480PktPoolOn;
}
