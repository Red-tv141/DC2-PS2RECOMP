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
    // G770-B: 8 MiB -> 32 MiB. A chain above the cap is freed outright and the next one re-grows
    // from a smaller buffer; map15's per-frame VIF1 chains reach that scale.
    constexpr size_t kG503MaxKeepBytes = 32u * 1024u * 1024u;

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

    // ⭐ G770-B: hand out the LARGEST pooled buffer, not the most recently released one.
    //
    // MEASURED (g770a, map15 uncapped, clean stack sampler, EE thread = the pole at 90 % busy once
    // G770-A removed the parse<->exec ping-pong): `_Insert_counted_range <- writeIORegister <-
    // sceDmaSend` 174/698 samples (25 %) and `free_base <- vector::_Change_array <-
    // writeIORegister` 66/698 (9.5 %, the ZwFreeVirtualMemory of a LARGE block). `_Change_array`
    // is a REALLOCATION: the chain outgrew its buffer. G503's pool is LIFO and shared by the GIF and
    // VIF1 channels, so a multi-megabyte VIF1 chain routinely received the small buffer a GIF chain
    // had just released, and re-grew geometrically from there — memmoving everything appended so
    // far at each step and VirtualFree-ing the outgrown block — exactly the G503 defect, re-created
    // by the pool's selection order. Best-capacity selection gives every chain the high-water
    // buffer; a small chain takes a big buffer at no cost (capacity is only address space until
    // touched, and the pool keeps it touched). EXACT: only CAPACITY differs, never a byte or a size.
    // Rollback DC2_G770_NO_CHAINFIT=1 (LIFO, the G503 behaviour).
    bool g770ChainFitOn()
    {
        static const bool s_on = []() {
            const char *v = std::getenv("DC2_G770_NO_CHAINFIT");
            return !(v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0);
        }();
        return s_on;
    }

    // G770-B census (DC2_G770_STAT=1): per staged chain, bytes and whether it outgrew the buffer it
    // was handed (a reallocation = memmove of everything so far + a free). Printed every 4096 chains.
    void g770NoteChain(size_t bytes, size_t cap0, size_t cap1, bool vif)
    {
        static const bool s_stat = []() {
            const char *v = std::getenv("DC2_G770_STAT");
            return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }();
        if (!s_stat)
            return;
        static uint64_t n = 0, grew = 0, sumBytes = 0, maxBytes = 0, vifN = 0;
        ++n;
        vifN += vif ? 1u : 0u;
        grew += (cap1 > cap0) ? 1u : 0u;
        sumBytes += bytes;
        maxBytes = std::max<uint64_t>(maxBytes, bytes);
        if ((n & 4095u) == 0u)
            std::fprintf(stderr, "[G770:chain] n=%llu vif=%llu grew=%llu (%.2f%%) avgKB=%.1f maxKB=%.1f poolSlots=%zu\n",
                         (unsigned long long)n, (unsigned long long)vifN, (unsigned long long)grew,
                         100.0 * (double)grew / (double)n, (double)sumBytes / (double)n / 1024.0,
                         (double)maxBytes / 1024.0, s_g503ChainPool.size());
    }

    std::vector<uint8_t> g503AcquireChainBuf()
    {
        if (!s_g503ChainPool.empty())
        {
            size_t pick = s_g503ChainPool.size() - 1u;
            if (g770ChainFitOn())
                for (size_t i = 0; i < s_g503ChainPool.size(); ++i)
                    if (s_g503ChainPool[i].capacity() > s_g503ChainPool[pick].capacity())
                        pick = i;
            std::vector<uint8_t> buf = std::move(s_g503ChainPool[pick]);
            s_g503ChainPool.erase(s_g503ChainPool.begin() + static_cast<std::ptrdiff_t>(pick));
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
