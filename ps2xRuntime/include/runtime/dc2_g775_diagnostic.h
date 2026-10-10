#pragma once

#if defined(PS2X_G775_DIAG)
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace dc2_g775_diag
{
inline std::atomic<uint64_t> starts{0}, resumes{0}, coreRuns{0};

inline bool ceilingOff()
{
    static const bool enabled = [] {
        const char *value = std::getenv("DC2_G775_CEILING_VU1_OFF");
        return value && std::atoi(value) == 1;
    }();
    return enabled;
}

// Every start and every resume share one immutable all-off diagnostic switch.
// No program is selected; no fallback VU execution survives this arm.
inline bool beforeExecution(bool resume, uint32_t pc)
{
    const uint64_t count = (resume ? resumes : starts).fetch_add(1, std::memory_order_relaxed) + 1;
    const bool off = ceilingOff();
    if (count == 1 || (count % 8192) == 0)
        std::fprintf(stderr, "[G775:ceiling] off=%u starts=%llu resumes=%llu coreRuns=%llu pc=%x\n",
                     unsigned(off), static_cast<unsigned long long>(starts.load()),
                     static_cast<unsigned long long>(resumes.load()),
                     static_cast<unsigned long long>(coreRuns.load()), pc);
    return off;
}

// Independent downstream entry instrumentation catches any missed entry path.
inline void noteCoreRun()
{
    coreRuns.fetch_add(1, std::memory_order_relaxed);
    if (ceilingOff())
    {
        std::fprintf(stderr, "[G775:ceiling-VIOLATION] VU1 core executed in all-off arm\n");
        std::abort();
    }
}
}
#else
namespace dc2_g775_diag
{
inline constexpr bool beforeExecution(bool, unsigned) { return false; }
inline constexpr void noteCoreRun() {}
}
#endif
