#include <cstdint>

#if defined(PS2X_G652_CRITICAL_TRACE)
extern const bool g652CriticalTraceOn;
void g652CriticalTrace(uint32_t kind, uint64_t value);
void g652CriticalFrameEnd();
#else
static constexpr bool g652CriticalTraceOn = false;
static inline void g652CriticalTrace(uint32_t, uint64_t) {}
static inline void g652CriticalFrameEnd() {}
#endif
