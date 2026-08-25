// G655 P14: normal-build contract for the default-off G343 census/closed suffix prototype.
static constexpr bool g343CensusOn() { return false; }
static constexpr bool g343L2lExactOn() { return false; }
static std::atomic<uint64_t> g_g343ConsumeAdmits{0};
template <typename... T> static void g343EstimateRows(T &&...) {}
template <typename... T> static void g343NoteL2l(T &&...) {}
