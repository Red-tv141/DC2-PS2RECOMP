// ============================================================================
// G645 — the EE/VIF1 half of G480's packet-buffer return path (ROADMAP Priority 5)
//
// WHY THIS EXISTS. G480 built a cross-thread RETURN path for GIF packet buffers: the GS worker
// stages every consumed `GifArbiterPacket::data` into a shared bin (one mutex per window) instead
// of freeing it. But only ONE producer was ever wired to the TAKE side — the VU1 worker's XGKICK
// Path1 capture. `GifArbiter::submit`, which is where the EE thread materialises every Path2/Path3
// packet the VIF1 chain produces, still did
//
//     pkt.data.resize(sizeBytes);            // fresh malloc + value-init (memset) of sizeBytes
//     std::memcpy(pkt.data.data(), data, sizeBytes);
//
// once per packet — 8,042 packets/frame on `dragon`. A fresh EE-thread PC profile of `s05` (the
// EE-poled route, `DC2_G503_EEPROF=1`) reads `PS2Memory::processVIF1Data` 12.50% with
// `RtlAllocateHeap` 1.62%, `std::vector<unsigned char>::_Insert_counted_range` 1.53%, and in the
// caller view `GifArbiter::submit` 0.56%, `operator new` 0.59%,
// `std::vector<GifArbiterPacket>::_Emplace_reallocate` 0.42%,
// `std::vector<unsigned char>::_Resize_reallocate` 0.32%. So the bin was being FED by every path and
// DRAINED by one; the buffers the EE thread needs were already sitting in it, being dropped at the
// cap.
//
// TWO ALLOCATIONS PER WINDOW ARE DELETED, NOT ONE.
//   1. the per-PACKET payload buffer  — taken from the G480 bin through a thread-local free list,
//      so the hot path is a `pop_back` plus an `assign` into memory that already has capacity;
//   2. the per-WINDOW `std::vector<GifArbiterPacket>` — `drain()` MOVES `m_queue` to the worker, so
//      the next window always started from capacity 0 and grew again (~4 reallocations per window,
//      ~1,560 windows/frame). A second bin hands the emptied outer vector back.
//
// EXACTNESS. `dst.assign(src, src + bytes)` leaves `data()`/`size()` bit-identical to
// `resize(bytes)` + `memcpy`; it merely skips the value-initialisation that `resize` performs and
// `memcpy` then overwrites. Only CAPACITY survives a recycle, and no consumer can observe it —
// `GifArbiter::ProcessPacketFn` is `void(const uint8_t *, uint32_t)` and is called synchronously,
// so a packet buffer's lifetime under the pool is exactly the lifetime it had before (it was freed
// at the same point; now it is moved to the bin at the same point). The outer vector is `clear()`ed
// before it is parked, so a recycled window starts `empty()` exactly as a fresh one did.
//
// BOUNDS. The take side refuses anything larger than `kG480MaxBufCap` (8 KB), so a Path3 IMAGE
// upload never comes out of the bin — the same filter the return side already applies. The window
// bin is hard-capped at `kG645MaxWinBin`.
//
// Rollback: DC2_G645_NO_EEPOOL=1 (restores the exact pre-G645 `resize` + `memcpy` and the moved
// `m_queue`). Census: DC2_G645_STAT=1 -> [G645:eepool].
// ============================================================================

// Default-ON. File-scope const, not a function-local static: this guard sits in a per-packet path
// on the EE thread, where a magic-static guard check is measurable (G268).
static const bool s_g645EePoolOn = (std::getenv("DC2_G645_NO_EEPOOL") == nullptr);
inline bool g645EePoolOn() { return s_g645EePoolOn; }

static const bool s_g645StatOn = (std::getenv("DC2_G645_STAT") != nullptr);

// EE-thread-only counters (the take side is single-threaded); the window counters are touched by
// both threads, so those are atomics.
static uint64_t g_g645Hits = 0ull, g_g645Miss = 0ull, g_g645Refills = 0ull, g_g645Big = 0ull;
static std::atomic<uint64_t> g_g645WinHits{0ull}, g_g645WinMiss{0ull};

// ---------------------------------------------------------------------------
// EE THREAD: materialise one packet payload, reusing a recycled buffer when one is available.
inline void g645TakePacketBuf(std::vector<uint8_t> &dst, const uint8_t *src, uint32_t bytes)
{
    if (s_g645EePoolOn && bytes <= kG480MaxBufCap)
    {
        static thread_local std::vector<std::vector<uint8_t>> s_eeLocal;
        if (s_eeLocal.empty())
        {
            G480PktBin &bin = g480PktBin();
            std::lock_guard<std::mutex> lk(bin.mtx);
            size_t n = bin.free.size();
            if (n > kG480RefillN)
                n = kG480RefillN;
            for (size_t i = 0u; i < n; ++i)
            {
                s_eeLocal.push_back(std::move(bin.free.back()));
                bin.free.pop_back();
            }
            if (s_g645StatOn)
                ++g_g645Refills;
        }
        if (!s_eeLocal.empty())
        {
            dst = std::move(s_eeLocal.back());
            s_eeLocal.pop_back();
            dst.assign(src, src + bytes); // no allocation while capacity suffices
            if (s_g645StatOn)
                ++g_g645Hits;
            return;
        }
        if (s_g645StatOn)
            ++g_g645Miss;
    }
    else if (s_g645StatOn && s_g645EePoolOn)
        ++g_g645Big;
    // Exact pre-G645 form (rollback arm, pool miss, or an IMAGE-sized payload).
    dst.resize(bytes);
    std::memcpy(dst.data(), src, bytes);
}

// ---------------------------------------------------------------------------
// The per-WINDOW outer vector. Defined once in ps2_gif_arbiter.cpp beside g480PktBin(), and leaked
// for the same reason: a window still in flight at teardown must always have a valid home.
struct G645WinBin
{
    std::mutex mtx;
    std::vector<std::vector<GifArbiterPacket>> free;
};
G645WinBin &g645WinBin();

static constexpr size_t kG645MaxWinBin = 512u; // ~1/3 of a frame's windows on `dragon`

// EE THREAD, drain(): fetch an EMPTY window vector that still owns its capacity.
inline void g645TakeWindowVec(std::vector<GifArbiterPacket> &dst)
{
    if (!s_g645EePoolOn)
        return;
    G645WinBin &bin = g645WinBin();
    std::lock_guard<std::mutex> lk(bin.mtx);
    if (bin.free.empty())
    {
        g_g645WinMiss.fetch_add(1ull, std::memory_order_relaxed);
        return;
    }
    dst = std::move(bin.free.back());
    bin.free.pop_back();
    g_g645WinHits.fetch_add(1ull, std::memory_order_relaxed);
}

// GS WORKER, after the window has been dispatched AND its payload buffers staged back: park the
// outer vector. `clear()` runs the packet destructors, but every `data` is already an empty husk
// (G480's return side moved it out), so it frees nothing and the capacity survives.
inline void g645GiveWindowVec(std::vector<GifArbiterPacket> &&v)
{
    if (!s_g645EePoolOn || v.capacity() == 0u)
        return;
    v.clear();
    G645WinBin &bin = g645WinBin();
    std::lock_guard<std::mutex> lk(bin.mtx);
    if (bin.free.size() >= kG645MaxWinBin)
        return;
    bin.free.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// Census (default OFF). Prices the site's call rate and the hit rate, so a miss-dominated pool can
// never be read as a neutral mechanism (G480's rule).
inline void g645NoteWindow()
{
    if (!s_g645StatOn)
        return;
    static thread_local uint64_t s_lastTick = 0ull, s_frames = 0ull;
    static thread_local uint64_t s_lastHits = 0ull, s_lastMiss = 0ull;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    if (tick == s_lastTick)
        return;
    s_lastTick = tick;
    if (++s_frames < 120ull)
        return;
    const double f = static_cast<double>(s_frames);
    const uint64_t dHits = g_g645Hits - s_lastHits;
    const uint64_t dMiss = g_g645Miss - s_lastMiss;
    size_t binNow = 0u, winNow = 0u;
    {
        G480PktBin &bin = g480PktBin();
        std::lock_guard<std::mutex> lk(bin.mtx);
        binNow = bin.free.size();
    }
    {
        G645WinBin &bin = g645WinBin();
        std::lock_guard<std::mutex> lk(bin.mtx);
        winNow = bin.free.size();
    }
    std::fprintf(stderr,
                 "[G645:eepool] frames=%llu pkts/f=%.0f hit=%.1f%% refills/f=%.1f big/f=%.0f "
                 "binNow=%zu winHit=%llu winMiss=%llu winNow=%zu\n",
                 static_cast<unsigned long long>(s_frames), (double)(dHits + dMiss) / f,
                 (dHits + dMiss) ? 100.0 * (double)dHits / (double)(dHits + dMiss) : 0.0,
                 (double)g_g645Refills / f, (double)g_g645Big / f, binNow,
                 static_cast<unsigned long long>(g_g645WinHits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g645WinMiss.load(std::memory_order_relaxed)),
                 winNow);
    s_frames = 0ull;
    g_g645Refills = 0ull;
    g_g645Big = 0ull;
    s_lastHits = g_g645Hits;
    s_lastMiss = g_g645Miss;
}
