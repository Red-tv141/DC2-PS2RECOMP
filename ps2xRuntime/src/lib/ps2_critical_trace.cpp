#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    constexpr uint64_t kCapacity = 1u << 16u;

    struct Event
    {
        std::atomic<uint64_t> published{0u};
        uint64_t ns = 0u;
        uint64_t value = 0u;
        uint32_t frame = 0u;
        uint32_t tid = 0u;
        uint16_t kind = 0u;
    };

    std::array<Event, kCapacity> s_events{};
    std::atomic<uint64_t> s_next{0u};
    std::atomic<uint32_t> s_frame{0u};

    uint64_t nowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint32_t threadId()
    {
#if defined(_WIN32)
        return static_cast<uint32_t>(GetCurrentThreadId());
#else
        return 0u;
#endif
    }

    uint32_t reportEvery()
    {
        static const uint32_t value = [] {
            const char *v = std::getenv("DC2_G652_CRIT_EVERY");
            const unsigned long n = v ? std::strtoul(v, nullptr, 0) : 30u;
            return static_cast<uint32_t>(std::max<unsigned long>(1u,
                                             std::min<unsigned long>(600u, n)));
        }();
        return value;
    }

    struct Stage
    {
        uint64_t firstQueue = UINT64_MAX;
        uint64_t firstBegin = UINT64_MAX;
        uint64_t lastEnd = 0u;
        uint64_t value = 0u;
        uint32_t queueN = 0u, beginN = 0u, endN = 0u;
    };

    double ms(uint64_t ns) { return static_cast<double>(ns) / 1.0e6; }
}

extern const bool g652CriticalTraceOn = [] {
    const char *v = std::getenv("DC2_G652_CRIT_TRACE");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}();

// Event kinds: VU queue/begin/end = 10/11/12, GS = 20/21/22, GL = 30/31/32, EE = 40/41/42.
// G653 P4 added the EE stage. Without it the trace could show when each CONSUMER ran but not when
// the producer that fed them started or finished, so no cross-thread handoff could be attributed:
// a GS "handoff" of 4 ms is a scheduling delay if EE finished early and simply producer latency if
// EE was still generating. The EE pair is emitted at the mgEndFrame boundary, the only guest-side
// frame edge the runtime owns.
void g652CriticalTrace(uint32_t kind, uint64_t value)
{
    if (!g652CriticalTraceOn)
        return;
    const uint64_t seq = s_next.fetch_add(1u, std::memory_order_relaxed);
    Event &e = s_events[seq & (kCapacity - 1u)];
    e.ns = nowNs();
    e.value = value;
    e.frame = s_frame.load(std::memory_order_relaxed);
    e.tid = threadId();
    e.kind = static_cast<uint16_t>(kind);
    e.published.store(seq + 1u, std::memory_order_release);
}

void g652CriticalFrameEnd()
{
    if (!g652CriticalTraceOn)
        return;
    const uint32_t frame = s_frame.load(std::memory_order_relaxed);
    const uint64_t endNs = nowNs();
    const uint64_t end = s_next.load(std::memory_order_acquire);
    const uint64_t begin = end > kCapacity ? end - kCapacity : 0u;
    Stage stages[4]{};
    uint64_t first = UINT64_MAX, last = 0u;
    uint32_t events = 0u;
    for (uint64_t seq = begin; seq < end; ++seq)
    {
        const Event &e = s_events[seq & (kCapacity - 1u)];
        if (e.published.load(std::memory_order_acquire) != seq + 1u || e.frame != frame)
            continue;
        const uint32_t stageIndex = e.kind >= 40u ? 3u : (e.kind >= 30u ? 2u : (e.kind >= 20u ? 1u : 0u));
        const uint32_t phase = e.kind % 10u;
        Stage &s = stages[stageIndex];
        if (phase == 0u)
        {
            s.firstQueue = std::min(s.firstQueue, e.ns);
            ++s.queueN;
        }
        else if (phase == 1u)
        {
            s.firstBegin = std::min(s.firstBegin, e.ns);
            ++s.beginN;
        }
        else if (phase == 2u)
        {
            s.lastEnd = std::max(s.lastEnd, e.ns);
            ++s.endN;
        }
        s.value += e.value;
        first = std::min(first, e.ns);
        last = std::max(last, e.ns);
        ++events;
    }

    if ((frame % reportEvery()) == 0u && events != 0u)
    {
        const char *names[4] = {"VU", "GS", "GL", "EE"};
        std::fprintf(stderr, "[G652:crit] frame=%u events=%u span=%.3fms", frame, events,
                     first != UINT64_MAX ? ms(last - first) : 0.0);
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            const Stage &s = stages[i];
            const double handoff = s.firstQueue != UINT64_MAX && s.firstBegin != UINT64_MAX &&
                                           s.firstBegin >= s.firstQueue
                                       ? ms(s.firstBegin - s.firstQueue)
                                       : 0.0;
            const double active = s.firstBegin != UINT64_MAX && s.lastEnd >= s.firstBegin
                                      ? ms(s.lastEnd - s.firstBegin)
                                      : 0.0;
            std::fprintf(stderr, " %s[q=%u b=%u e=%u hand=%.3f active=%.3f]",
                         names[i], s.queueN, s.beginN, s.endN, handoff, active);
        }
        const double tail = last != 0u && endNs >= last ? ms(endNs - last) : 0.0;
        std::fprintf(stderr, " tail=%.3fms\n", tail);
    }
    s_frame.store(frame + 1u, std::memory_order_release);
}
