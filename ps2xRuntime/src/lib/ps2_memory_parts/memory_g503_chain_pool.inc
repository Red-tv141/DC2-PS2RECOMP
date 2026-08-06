// ===== G503: recycled DMA-chain staging buffer =====
//
// PREMISE, measured on the pole thread this phase discovered. G503's premise gate read the three
// existing derivative probes at VU1 0.04 / GS 0.27 / GPU ~0.00 — sum 0.31 against G502's 0.85 — so
// two thirds of the frame's marginal cost was in a unit no phase in this arc had ever injected into.
// `eeslow` (the fourth probe, G503 §3) ranks the EE guest thread at **0.37, sign 4/4, 0 of 25
// windows favouring the arm that adds 2 ms/f** — the top-ranked unit on this binary. `[G182:ee]
// cpuMs/f` (a GetThreadTimes delta, not the `busyMs/f` frame bracket beside it) puts 16.7-19.3 ms of
// REAL CPU on that thread per ~20 ms frame.
//
// WHERE IT GOES. `DC2_G503_EEPROF=1` (the G446 sampler's third instance) plus the caller attribution
// this phase added to it:
//
//   10.86% memmove                                          <- leaf
//   13.78% std::vector<uint8_t>::_Insert_counted_range      <- OUR call site for that leaf
//    5.69% ZwFreeVirtualMemory + 1.59% RtlAllocateHeap + 1.43% operator new + 0.71% _Resize_reallocate
//
// MECHANISM. `PS2Memory::processTransferChain` declares `std::vector<uint8_t> chainBuf;` FRESH for
// every DMA chain, with no reserve(), and then appends each tag's payload with
// `chainBuf.insert(chainBuf.end(), ...)` across up to kMaxChainTags = 16384 tags. A vector grown
// from zero reallocates geometrically, and every reallocation memmoves EVERYTHING accumulated so far
// — so a chain of N bytes moves ~2N bytes instead of N, and pays one operator new plus one free per
// chain on top. The staged bytes are the game's whole per-frame display list, which on lean MAP-0 is
// megabytes (`[G498:§4]`: 3,936 KB/frame of IMAGE payload alone).
//
// THE LEVER. Keep the buffers in a thread-local free list and hand out one with its capacity already
// at the high-water mark. After a few frames no chain reallocates at all, and no chain allocates or
// frees. The appends themselves — the real data movement — are untouched.
//
// EXACTNESS. The only difference is that `chainBuf` starts with non-zero CAPACITY instead of zero.
// `insert(end(), first, last)` appends the same bytes in the same order either way, `size()` is
// identical, and `pt.chainData = std::move(chainBuf)` hands the consumer the same bytes at the same
// length. `processVIF1Data(p.chainData.data(), p.chainData.size())` therefore sees a byte-identical
// stream. Bit-exact by construction — there is no arm-dependent output to verify, only a
// capacity.
//
// THREADING. The pool is `thread_local`, so acquire and release need not happen on the same thread:
// a buffer acquired on one and released on another simply migrates between two private pools, and
// there is no shared state and no lock. (In practice both sites run on the EE thread — the chain
// walk from the CHCR write, the release from processPendingTransfers, both under sceDmaSend.)
//
// WHY A CAP. Retaining capacity forever would pin the largest chain the run ever saw. Buffers larger
// than kG503MaxKeepBytes are released with their storage freed instead of pooled, so a single
// pathological chain (a cutscene, an overlay load) cannot permanently inflate the resident set.
//
// Arm:      DC2_G419_AB=chainpool (paired, hold 16 — thread-level, it changes allocator traffic)
// Rollback: DC2_G503_NO_CHAINPOOL=1
// Status:   default-ON

// G503: the arm selector lives in the rasterizer TU with every other one (g419_ab_instrument.inc).
// Same declaration style as ps2_gif_arbiter.cpp's `extern int g438SlowArm(int)`.
extern int g503ChainPoolArm();

namespace
{
    // 8 buffers is more than the deepest observed nesting of staged chains per frame (VIF1 and GIF
    // each stage at most a handful before processPendingTransfers drains both lists).
    constexpr size_t kG503PoolSlots = 8;
    constexpr size_t kG503MaxKeepBytes = 8u * 1024u * 1024u;

    thread_local std::vector<std::vector<uint8_t>> s_g503ChainPool;

    bool g503ChainPoolEnabled()
    {
        // G500's law: this is read once, in a function-local `static const`. An uncached getenv here
        // would sit on the DMA-chain path of the pole thread — the exact shape that cost ~8 ms/f on
        // the VU1 path before G500 memoised it.
        static const bool s_on = []() {
            const char *v = std::getenv("DC2_G503_NO_CHAINPOOL");
            return !(v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0);
        }();
        const int arm = g503ChainPoolArm();
        return (arm >= 0) ? (arm == 1) : s_on;
    }

    std::vector<uint8_t> g503AcquireChainBuf()
    {
        if (!s_g503ChainPool.empty())
        {
            std::vector<uint8_t> buf = std::move(s_g503ChainPool.back());
            s_g503ChainPool.pop_back();
            buf.clear(); // keeps capacity — this is the whole point
            return buf;
        }
        return std::vector<uint8_t>();
    }

    void g503ReleaseChainBuf(std::vector<uint8_t> &buf)
    {
        if (buf.capacity() == 0u || buf.capacity() > kG503MaxKeepBytes ||
            s_g503ChainPool.size() >= kG503PoolSlots)
        {
            std::vector<uint8_t>().swap(buf); // free it outright
            return;
        }
        buf.clear();
        s_g503ChainPool.push_back(std::move(buf));
    }
}
