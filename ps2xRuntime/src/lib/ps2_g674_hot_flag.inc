// G674: process-constant hot flags must collapse to one byte load at each call site.
// Function-local statics make MSVC emit a thread-safe-static TLS epoch check on every read,
// even after PGO inlines the accessor. DC2 launchers set the environment before process start;
// the explicitly live DC2_G268_LIVE_ENVREAD fallback still performs fresh getenv calls.
#if defined(_MSC_VER)
#define DC2_G674_HOT_FLAG_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define DC2_G674_HOT_FLAG_INLINE inline __attribute__((always_inline))
#else
#define DC2_G674_HOT_FLAG_INLINE inline
#endif
