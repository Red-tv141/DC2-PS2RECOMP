// G652 cold runtime helpers. Keep OS headers and measurement-only state out of ps2_runtime.cpp,
// which contains the hot dispatch and memory paths.

#include <atomic>
#include <cstdint>

#ifdef _WIN32
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
    std::atomic<uint64_t> g_eeWaitCpuNs{0ull};
    thread_local uint32_t g_eeWaitDepth = 0u;
    thread_local uint64_t g_eeWaitStartNs = 0ull;

    uint64_t currentThreadCpuNs()
    {
#ifdef _WIN32
        FILETIME creation, exit, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
            return 0ull;
        const uint64_t k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        const uint64_t u = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        return (k + u) * 100ull;
#else
        return 0ull;
#endif
    }
}

void ps2EeWaitScopeEnter()
{
    if (g_eeWaitDepth++ == 0u)
        g_eeWaitStartNs = currentThreadCpuNs();
}

void ps2EeWaitScopeExit()
{
    if (g_eeWaitDepth == 0u)
        return;
    if (--g_eeWaitDepth == 0u)
    {
        const uint64_t now = currentThreadCpuNs();
        if (now >= g_eeWaitStartNs)
            g_eeWaitCpuNs.fetch_add(now - g_eeWaitStartNs, std::memory_order_relaxed);
    }
}

uint64_t ps2EeWaitCpuNs()
{
    return g_eeWaitCpuNs.load(std::memory_order_relaxed);
}
