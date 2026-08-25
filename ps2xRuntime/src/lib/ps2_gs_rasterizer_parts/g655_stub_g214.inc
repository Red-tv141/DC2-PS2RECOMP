// G655 P17: closed missing-cap trace.  Names remain declared because the discarded diagnostic
// block in drawTriangle is still parsed even though the constexpr gate folds it away.
static constexpr bool g214CapTraceEnabled() { return false; }
static std::mutex g_g214Mutex;
static uint64_t g_g214Tick = 0u;
static uint64_t g_g214Cap = 0u, g_g214Face = 0u, g_g214Hair = 0u;
static uint64_t g_g214Head = 0u, g_g214Body = 0u, g_g214Neck = 0u;
static void g214CapFlushLocked(uint64_t) {}
