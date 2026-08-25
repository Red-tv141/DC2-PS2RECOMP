// G655 P15: normal-build contract for the default-off G342 census/closed CT32 prototype.
static constexpr bool g342CensusOn() { return false; }
static constexpr bool g342Ct32BindOn() { return false; }
static std::atomic<uint64_t> g_g342Ct32Binds{0};
template <typename... T> static void g342NoteTexAlias(T &&...) {}
