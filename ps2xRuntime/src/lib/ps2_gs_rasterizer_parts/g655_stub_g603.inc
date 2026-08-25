// G655 P16: admission dry-run doubles classification work and belongs only in diagnostic builds.
static constexpr bool g603AdmitCensusOn() { return false; }
template <typename... T> static void g603NoteFallbackAdmission(T &&...) {}
