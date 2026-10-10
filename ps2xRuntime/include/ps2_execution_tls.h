#pragma once

#include <cstdint>

// Shared by generated dispatch/preemption and the runtime's slow paths and crash reporter.
struct Ps2DispatchRing
{
    uint32_t pcs[64];
    uint32_t next;
    uint32_t wrapped;
};

// MSVC requires a visible initializing definition to omit dynamic TLS access guards; extern
// constinit declarations alone still produce them. Inline linkage keeps one object per thread.
inline constinit thread_local Ps2DispatchRing g_ps2DispatchRing{};
inline constinit thread_local uint32_t g_ps2BackEdgeCounter{};
