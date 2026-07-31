// ============================================================================
// G440 — present-latch profile (diagnostic, DEFAULT OFF: DC2_G440_LATCH=1).
//
// G439 closed the GS window-count vein and left a ~5.4 ms/f "serial residue"
// (frame 47.6 - max(VU1 42.1, GS-window 42.3)) unattributed. G440 measured it
// with DC2_G184_BOUNDARY_STAT=1: the GS worker's frame-boundary closure costs
// 5.4-5.8 ms/f, so the GS worker's TOTAL occupancy is window 42.3 + boundary
// 5.5 = 47.8 ~= the frame. The residue IS the present latch, it sits on the
// pole thread, it is strictly serial (it runs after every window of the frame),
// and it has ~5.7 ms of headroom before it hits the VU1 co-limit at 42.1.
//
// This instrument splits the closure into its per-pixel components so a lever
// is aimed at the component that actually costs, not at the closure as a whole.
// Behaviour-pure: every timer is behind one cached env read and the default
// path does zero clock work.
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    enum G440LatchSlot
    {
        kG440SlotTotal = 0,   // whole GS::latchHostPresentationFrame
        kG440SlotCopy,        // GS::copyFrameToHostRgbaUnlocked (VRAM deswizzle read)
        kG440SlotAssign,      // its outPixels.assign() zero-fill
        kG440SlotNonBlack,    // countNonBlackPixels scans
        kG440SlotMerge,       // dual-CRT merge / blend pixel loops
        kG440SlotAlpha,       // normalizePresentationAlpha + applyFieldPresentation
        kG440SlotFlush,       // g144FlushPending inside the same frame-boundary closure
        kG440SlotCount
    };

    std::atomic<uint64_t> g_g440LatchNs[kG440SlotCount] = {};
    std::atomic<uint64_t> g_g440LatchCnt[kG440SlotCount] = {};

    inline bool g440LatchProfOn()
    {
        static const bool on = (std::getenv("DC2_G440_LATCH") != nullptr);
        return on;
    }

    struct G440LatchScope
    {
        int slot;
        bool on;
        std::chrono::steady_clock::time_point t0;

        explicit G440LatchScope(int s)
            : slot(s),
              on(g440LatchProfOn()),
              t0(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
        {
        }

        ~G440LatchScope()
        {
            if (!on)
                return;
            g_g440LatchNs[slot].fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count()),
                std::memory_order_relaxed);
            g_g440LatchCnt[slot].fetch_add(1ull, std::memory_order_relaxed);
        }
    };

    // Called once per latch (from the tail of GS::latchHostPresentationFrame's total scope).
    inline void g440LatchReport(const char *path, uint32_t w, uint32_t h)
    {
        if (!g440LatchProfOn())
            return;
        static uint64_t s_n = 0ull;
        static uint64_t s_lastNs[kG440SlotCount] = {};
        static uint64_t s_lastCnt[kG440SlotCount] = {};
        if ((++s_n % 30ull) != 0ull)
            return;
        double ms[kG440SlotCount];
        double per[kG440SlotCount];
        for (int i = 0; i < kG440SlotCount; ++i)
        {
            const uint64_t ns = g_g440LatchNs[i].load(std::memory_order_relaxed);
            const uint64_t c = g_g440LatchCnt[i].load(std::memory_order_relaxed);
            ms[i] = static_cast<double>(ns - s_lastNs[i]) / 1e6 / 30.0;
            per[i] = static_cast<double>(c - s_lastCnt[i]) / 30.0;
            s_lastNs[i] = ns;
            s_lastCnt[i] = c;
        }
        std::fprintf(stderr,
                     "[G440:latch] n=%llu path=%s size=%ux%u totalMs/f=%.2f | copy=%.2f/%.1f "
                     "(assign=%.2f) nonblack=%.2f/%.1f merge=%.2f/%.1f alpha=%.2f/%.1f "
                     "g144flush=%.2f/%.1f\n",
                     static_cast<unsigned long long>(s_n), path, w, h,
                     ms[kG440SlotTotal],
                     ms[kG440SlotCopy], per[kG440SlotCopy],
                     ms[kG440SlotAssign],
                     ms[kG440SlotNonBlack], per[kG440SlotNonBlack],
                     ms[kG440SlotMerge], per[kG440SlotMerge],
                     ms[kG440SlotAlpha], per[kG440SlotAlpha],
                     ms[kG440SlotFlush], per[kG440SlotFlush]);
    }
} // namespace

// Cross-TU bridges: the frame-boundary closure that owns the latch lives in
// dc2_game_override_parts/frame_end_and_core_helpers.inc and also runs g144FlushPending().
bool g440_latch_prof_on() { return g440LatchProfOn(); }
void g440_note_flush_ns(uint64_t ns)
{
    if (!g440LatchProfOn())
        return;
    g_g440LatchNs[kG440SlotFlush].fetch_add(ns, std::memory_order_relaxed);
    g_g440LatchCnt[kG440SlotFlush].fetch_add(1ull, std::memory_order_relaxed);
}
