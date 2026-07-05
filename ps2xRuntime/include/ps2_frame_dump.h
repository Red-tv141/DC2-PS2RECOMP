#pragma once

#include <cstdint>

class PS2Runtime;

namespace dc2 {

// Returns true if a file was written.
bool dumpFramePpm(uint64_t tick, bool forceBlank, PS2Runtime* rt);

} // namespace dc2
