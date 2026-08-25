// G655 P13: normal-build contract for the default-off texture-decode census.  The promoted
// decode-buffer pool remains in g620_decode_buffers.inc; the PSMCT32 row exactness oracle also
// remains live here because it validates shipped behavior rather than census bookkeeping.
static constexpr bool g620TexDecOn() { return false; }
static void g620NoteTexVisit(bool) {}
template <typename... T> static void g620NoteTexHit(bool, T &&...) {}
template <typename... T> static void g620NoteTexDecode(bool, T &&...) {}
template <typename... T> static void g620NoteTexDecodeNs(bool, T &&...) {}
template <typename... T> static void g620NoteStage(bool, T &&...) {}
static void g620TexDecReport(bool) {}

static void g620RowVerifyReport()
{
    static const bool on = envFlagEnabled("DC2_G620_VERIFY");
    if (!on)
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 2000u) != 0u)
        return;
    std::fprintf(stderr, "[G620:rowverify] flushes=%llu checked=%llu bad=%llu\n",
                 static_cast<unsigned long long>(s_n),
                 static_cast<unsigned long long>(
                     GSMem::g_g620RowVerifyN.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     GSMem::g_g620RowVerifyBad.load(std::memory_order_relaxed)));
}
