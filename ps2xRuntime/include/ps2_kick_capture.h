#pragma once

#include <cstdint>
#include <vector>

// Non-owning, per-worker kick state shared by the runtime and VU interpreter. Visible constant
// definitions prevent MSVC from guarding every cross-TU access with dynamic TLS initialization.
// The owners still install/clear these pointers at the same ordered kick boundaries.
inline constinit thread_local std::vector<std::vector<uint8_t>> *g_g297KickCapture = nullptr;
inline constinit thread_local std::vector<uint32_t> *g_g541KickAddresses = nullptr;
inline constinit thread_local uint32_t g_g297KickTop = 0u;
