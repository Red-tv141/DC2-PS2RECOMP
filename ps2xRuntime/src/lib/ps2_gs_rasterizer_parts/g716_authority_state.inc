// G716: memory authority is independent of a surface's pending render rows.
// Raw pages contain the complete, swizzled page, including unchanged guest bytes.
// A CPU publication copies those pages without changing their logical generation.
static const bool s_g716Authority = envFlagEnabled("DC2_G716_AUTHORITY") &&
                                    !envFlagEnabled("DC2_G716_NO_AUTHORITY");
static const bool s_g716Stat = envFlagEnabled("DC2_G716_STAT");
static const bool s_g716CpuView = envFlagEnabled("DC2_G716_CPUVIEW");
static const bool s_g716CpuSeed = envFlagEnabled("DC2_G716_CPUSEED");
static const bool s_g716Verify = envFlagEnabled("DC2_G716_VERIFY");
static const bool s_g716RawUpload = envFlagEnabled("DC2_G716_RAWUPLOAD");
// Producer bisect. Raw pages have exactly two producers: the framebuffer row handoff
// (g716RetireTarget) and the GPU-to-GPU raw copy (g716ExecuteL2l). Disabling one leaves the other
// fully GPU-consumed, so unlike the CPUVIEW/CPUSEED consumer split it does NOT change what the
// remaining producer's population is -- the consumer split force-publishes the framebuffer and
// thereby empties raw over exactly the pages it claims to be testing.
static const bool s_g716NoRetire = envFlagEnabled("DC2_G716_NORETIRE");
static const bool s_g716NoL2l = envFlagEnabled("DC2_G716_NOL2L");
// The publication this handoff replaces writes the WHOLE dirtyLo..dirtyHi interval, full width.
// The handoff ingests only rows carrying an exact row bit. Any row inside the interval without a
// bit is therefore content the publication would have written and the handoff writes NOWHERE --
// and g716ClearRows then retires the debt for it regardless. FULLSPAN makes the ingest cover
// exactly what the publication covered; the rowgap census prices the difference.
static const bool s_g716FullSpan = envFlagEnabled("DC2_G716_FULLSPAN");
// ⭐ The READER test. The audit proves no writer supersedes a raw page; it is structurally blind
// to a CONSUMER that reads guest VRAM for a raw page without being routed through the raw image.
// MIRROR publishes every raw page into guest VRAM at each flush while KEEPING the raw claim, so
// the GPU population and every ownership predicate are bit-for-bit unchanged and the ONLY thing
// that differs is what an unrouted guest-VRAM reader would see. Image correct => such a reader
// exists and is the defect. Image unchanged => guest-VRAM readers are exonerated.
// Deliberately returns the readback in full; correctness-only, never a timing candidate.
static const bool s_g716Mirror = envFlagEnabled("DC2_G716_MIRROR");
static uint64_t g_g716Mirrors = 0, g_g716MirrorPages = 0;
// With MIRROR holding guest VRAM correct at all times, NOCONSUME turns off every raw CONSUMER
// (texture views, the framebuffer seed overlay, the T8 atlas page restore). The renderer is then
// the shipped one in every respect except that g261Materialize has been replaced by the row
// handoff. Still wrong => the handoff damages the FBO or the residency, not memory.
static const bool s_g716NoConsume = envFlagEnabled("DC2_G716_NOCONSUME");
// Consumer bisect: bit0 = GPU texture views, bit1 = framebuffer seed overlay, bit2 = T8 atlas
// page restore. Default 7 (all). NOCONSUME forces 0.
static const uint32_t s_g716ConsumeMask = [] {
    if (envFlagEnabled("DC2_G716_NOCONSUME")) return 0u;
    const char *v = std::getenv("DC2_G716_CONSUME");
    return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 15u;
}();
static bool g716Consumes(uint32_t bit) { return (s_g716ConsumeMask & bit) != 0u; }
// ⭐ The raw image is a SINGLE MUTABLE GLOBAL with no notion of "when". A view decoded lazily on
// the worker reads whatever later producers have already written, not the content at the draw's
// own position in the stream. The drained oracle measures exactly this: 2.8% of texels disagree
// with the shipped decode until a drain is forced before the reference, after which bad=0 over
// 268 M checks. SYNCVIEW pins each decode to its submit position by draining right after it.
// Correctness probe only -- a real fix must pin without a CPU round trip.
static const bool s_g716SyncView = envFlagEnabled("DC2_G716_SYNCVIEW");
// Refuse a raw view for any texture that overlaps a live G261 render target: that surface's
// authority is its FBO, which the shipped path binds directly.
static const bool s_g716NoRtt = envFlagEnabled("DC2_G716_NORTT");
// Collect the framebuffer seed's raw-page mask beside the fbPixels pack instead of at submit.
static const bool s_g716SeedAtPack = !envFlagEnabled("DC2_G716_SEED_AT_SUBMIT");
static uint64_t g_g716GapRetires = 0, g_g716GapRows = 0;
static uint64_t g_g716Uploads = 0, g_g716UploadWords = 0;
static std::array<uint64_t, 9> g_g716CpuCause{};
static std::array<uint8_t, 512> g_g716Raw{};
static uint32_t g_g716RawCount = 0;
static bool g_g716GpuObserver = false;
static uint64_t g_g716Retires = 0, g_g716Copies = 0, g_g716Views = 0;
static uint64_t g_g716CpuBreaks = 0, g_g716CpuPages = 0, g_g716Seeds = 0;
static uint64_t g_g716Rows = 0, g_g716Fallbacks = 0;
// ⭐ view key -> the source pages' generation stamp AT THE TIME THE VIEW WAS DECODED.
// `st.texKey` is built from TEX0 register fields alone (tbp0/tbw/tw/th) and carries NO content
// component, so a set keyed on it can never notice that the bytes under those registers changed.
// It saturated at 76,286 builds while the route kept drawing and retiring, after which every raw
// texture was served from a permanently stale GPU view. The stamp restores the shipped
// invalidation contract: rebuild exactly when a source page generation moves.
static std::unordered_map<uint64_t, uint64_t> g_g716Built;
static const bool s_g716NoViewCache = envFlagEnabled("DC2_G716_NOVIEWCACHE");
static uint64_t g_g716ViewHits = 0, g_g716ViewStale = 0;

// ── The two-live-writers audit ──────────────────────────────────────────────────────────────────
// Every oracle in G714/G715/G716 so far compares the mechanism against ITSELF (raw vs raw, or an
// expectation rebuilt out of the same raw image), so all of them are blind to a SECOND writer
// still publishing the same address range into guest VRAM. This one is not: when a page becomes
// raw-authoritative its guest bytes are frozen, so guest VRAM for that page must not change again
// until the claim is retired. Any change means two representations are live and neither was
// retired -- exactly the escaped-transition class. Independent of the transforms by construction.
static const bool s_g716Audit = envFlagEnabled("DC2_G716_AUDIT");
static int g_g716WriterTag = 0; // set by a suspect guest-VRAM writer around its publication
static std::array<uint64_t, 512> g_g716RawStamp{};
static std::array<uint8_t, 512> g_g716RawStampOk{};
static std::array<int, 512> g_g716RawStampTag{};
static uint64_t g_g716AuditChecks = 0, g_g716AuditBad = 0, g_g716AuditReports = 0;
static std::array<uint64_t, 24> g_g716AuditByTag{};   // consumer that noticed
static std::array<uint64_t, 24> g_g716AuditByWriter{}; // writer tag active at the last stamp

static const char *kG716Tag[24] = {
    "none", "display-pub", "g289-owner", "g272", "cpu-replay", "texture", "seed", "publish",
    "shadow", "upload", "mat-inline", "t8-restore", "12", "13", "14", "15",
    "16", "17", "18", "19", "20", "21", "22", "23"};

static uint64_t g716PageDigest(uint32_t page)
{
    if (!g_g144LastVram || (uint64_t(page) + 1u) * 8192u > g_g261VramSize) return 0u;
    const uint64_t *w = reinterpret_cast<const uint64_t *>(g_g144LastVram + page * 8192u);
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0; i < 1024u; ++i) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}
static void g716StampPage(uint32_t page)
{
    if (!s_g716Audit || page >= 512) return;
    g_g716RawStamp[page] = g716PageDigest(page);
    g_g716RawStampOk[page] = 1u;
    g_g716RawStampTag[page] = g_g716WriterTag;
}
// `tag` names the CONSUMER about to trust the raw claim. Reports the page, the consumer, and the
// writer tag that was active when the page was stamped, then re-stamps so one escape is not
// counted forever.
static void g716AuditPage(uint32_t page, int tag)
{
    if (!s_g716Audit || page >= 512 || !g_g716Raw[page] || !g_g716RawStampOk[page]) return;
    ++g_g716AuditChecks;
    const uint64_t now = g716PageDigest(page);
    if (now == g_g716RawStamp[page]) return;
    ++g_g716AuditBad;
    g_g716AuditByTag[std::min(23, std::max(0, tag))]++;
    g_g716AuditByWriter[std::min(23, std::max(0, g_g716RawStampTag[page]))]++;
    if (++g_g716AuditReports <= 24 || (g_g716AuditReports % 4096) == 0)
        std::fprintf(stderr, "[G716:audit] BAD n=%llu page=%u consumer=%s wroteUnder=%s "
                     "checks=%llu bad=%llu\n",
                     (unsigned long long)g_g716AuditReports, page,
                     kG716Tag[std::min(23, std::max(0, tag))],
                     kG716Tag[std::min(23, std::max(0, g_g716RawStampTag[page]))],
                     (unsigned long long)g_g716AuditChecks, (unsigned long long)g_g716AuditBad);
    g_g716RawStamp[page] = now; // re-arm; count transitions, not their persistence
}
static void g716AuditSweep(int tag)
{
    if (!s_g716Audit || g_g716RawCount == 0) return;
    for (uint32_t p = 0; p < 512u; ++p) if (g_g716Raw[p]) g716AuditPage(p, tag);
}
static void g716AuditRange(const G144BlockRange &r, int tag)
{
    if (!s_g716Audit || g_g716RawCount == 0 || !r.valid) return;
    for (uint32_t p = r.first >> 5u; p <= std::min(511u, r.last >> 5u); ++p)
        if (g_g716Raw[p]) g716AuditPage(p, tag);
}
// Direct escaped-writer detector: called by a guest-VRAM byte writer BEFORE it writes. Any raw
// page inside its rect is a live claim it is about to supersede without retiring, so this names
// the writer exactly rather than inferring it from a later consumer.
static std::array<uint64_t, 24> g_g716EscapeByWriter{};
static uint64_t g_g716EscapeReports = 0;
// Per-`g570Source` tally from the declared-write hook (g178BumpRectImpl), so a writer that bumps
// generations is named by its own source id rather than by a hand-placed tag.
static std::array<uint64_t, 32> g_g716WriterCounts{}, g_g716WriterRawPages{};
static void g716NoteGuestWrite(uint32_t block, uint32_t bw, uint32_t psm,
                               uint32_t x, uint32_t y, uint32_t w, uint32_t h, int tag)
{
    if (!s_g716Audit || g_g716RawCount == 0 || !w || !h) return;
    const auto r = g144RangeForRect(block, static_cast<uint8_t>(psm), bw, x, y, w, h);
    if (!r.valid) return;
    uint32_t hits = 0, firstPage = 0;
    for (uint32_t p = r.first >> 5u; p <= std::min(511u, r.last >> 5u); ++p)
        if (g_g716Raw[p]) { if (!hits) firstPage = p; ++hits; }
    if (!hits) return;
    g_g716EscapeByWriter[std::min(23, std::max(0, tag))] += hits;
    if (++g_g716EscapeReports <= 24 || (g_g716EscapeReports % 8192) == 0)
        std::fprintf(stderr, "[G716:escape] n=%llu writer=%s rawPages=%u firstPage=%u "
                     "block=%x bw=%u psm=%x xy=%u,%u wh=%u,%u\n",
                     (unsigned long long)g_g716EscapeReports,
                     kG716Tag[std::min(23, std::max(0, tag))], hits, firstPage,
                     block, bw, psm, x, y, w, h);
}

// Scoped writer tag: names whoever is about to write guest VRAM, so a stamp can record it.
struct G716WriterScope
{
    int prev;
    explicit G716WriterScope(int tag) : prev(g_g716WriterTag) { g_g716WriterTag = tag; }
    ~G716WriterScope() { g_g716WriterTag = prev; }
};

static bool g716AnyRaw() { return s_g716Authority && g_g716RawCount != 0; }
static void g716SetRaw(uint32_t page, bool raw)
{
    if (page >= g_g716Raw.size() || (g_g716Raw[page] != 0) == raw) return;
    g_g716Raw[page] = raw ? 1u : 0u;
    if (raw) { ++g_g716RawCount; g716StampPage(page); }
    else { --g_g716RawCount; g_g716RawStampOk[page] = 0u; }
}
static bool g716RangeHasRaw(const G144BlockRange &range)
{
    if (!g716AnyRaw() || !range.valid) return false;
    for (uint32_t p = range.first >> 5u; p <= std::min(511u, range.last >> 5u); ++p)
        if (g_g716Raw[p]) return true;
    return false;
}
static void g716Report()
{
    if (!s_g716Stat) return;
    std::fprintf(stderr, "[G716:authority] copies=%llu retire=%llu rows=%llu raw=%u "
                 "views=%llu seeds=%llu cpuBreaks=%llu cpuPages=%llu fallback=%llu\n",
                 (unsigned long long)g_g716Copies, (unsigned long long)g_g716Retires,
                 (unsigned long long)g_g716Rows, g_g716RawCount,
                 (unsigned long long)g_g716Views, (unsigned long long)g_g716Seeds,
                 (unsigned long long)g_g716CpuBreaks, (unsigned long long)g_g716CpuPages,
                 (unsigned long long)g_g716Fallbacks);
    std::fprintf(stderr, "[G716:cpu] replay=%llu inline=%llu upload=%llu local=%llu l2h=%llu "
                 "tex=%llu resync=%llu fail=%llu clut=%llu\n",
                 (unsigned long long)g_g716CpuCause[0], (unsigned long long)g_g716CpuCause[1],
                 (unsigned long long)g_g716CpuCause[2], (unsigned long long)g_g716CpuCause[3],
                 (unsigned long long)g_g716CpuCause[4], (unsigned long long)g_g716CpuCause[5],
                 (unsigned long long)g_g716CpuCause[6], (unsigned long long)g_g716CpuCause[7],
                 (unsigned long long)g_g716CpuCause[8]);
    std::fprintf(stderr, "[G716:upload] transfers=%llu words=%llu\n",
                 (unsigned long long)g_g716Uploads, (unsigned long long)g_g716UploadWords);
    std::fprintf(stderr, "[G716:view] builds=%llu hits=%llu staleRebuilds=%llu keys=%zu\n",
                 (unsigned long long)g_g716Views, (unsigned long long)g_g716ViewHits,
                 (unsigned long long)g_g716ViewStale, g_g716Built.size());
    if (!s_g716Audit) return;
    char buf[512];
    int off = 0;
    for (int i = 0; i < 24 && off < 400; ++i)
        if (g_g716AuditByTag[i])
            off += std::snprintf(buf + off, sizeof(buf) - off, " %s=%llu", kG716Tag[i],
                                 (unsigned long long)g_g716AuditByTag[i]);
    std::fprintf(stderr, "[G716:audit] checks=%llu bad=%llu consumer:%s\n",
                 (unsigned long long)g_g716AuditChecks, (unsigned long long)g_g716AuditBad,
                 off ? buf : " none");
    off = 0;
    for (int i = 0; i < 24 && off < 400; ++i)
        if (g_g716EscapeByWriter[i])
            off += std::snprintf(buf + off, sizeof(buf) - off, " %s=%llu", kG716Tag[i],
                                 (unsigned long long)g_g716EscapeByWriter[i]);
    std::fprintf(stderr, "[G716:escape] events=%llu byWriter(rawPagesSuperseded):%s\n",
                 (unsigned long long)g_g716EscapeReports, off ? buf : " none");
    off = 0;
    for (int i = 0; i < 32 && off < 400; ++i)
        if (g_g716WriterRawPages[i])
            off += std::snprintf(buf + off, sizeof(buf) - off, " src%d=%llu/%llupg", i,
                                 (unsigned long long)g_g716WriterCounts[i],
                                 (unsigned long long)g_g716WriterRawPages[i]);
    std::fprintf(stderr, "[G716:declared] bumpsOnRaw(calls/rawPages):%s\n", off ? buf : " none");
}

// The interval is now only a compatibility bound. Exact row membership is the debt.
static void g716MarkRows(G261Res &r, int lo, int hi)
{
    if (!s_g716Authority) return;
    if (!r.dirty) r.g716Rows.fill(0);
    for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
        r.g716Rows[y >> 6] |= uint64_t(1) << (y & 63);
}
static bool g716HasRows(const G261Res &r, int lo, int hi)
{
    if (!r.dirty) return false;
    if (!s_g716Authority) return hi >= r.dirtyLo && lo <= r.dirtyHi;
    for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
        if (r.g716Rows[y >> 6] & (uint64_t(1) << (y & 63))) return true;
    return false;
}
static void g716ClearRows(G261Res &r, int lo, int hi)
{
    for (int y = std::max(0, lo); y <= std::min(511, hi); ++y)
        r.g716Rows[y >> 6] &= ~(uint64_t(1) << (y & 63));
    r.dirtyLo = 512; r.dirtyHi = -1;
    for (int y = 0; y < 512; ++y)
        if (r.g716Rows[y >> 6] & (uint64_t(1) << (y & 63)))
        { r.dirtyLo = std::min(r.dirtyLo, y); r.dirtyHi = y; }
    r.dirty = r.dirtyHi >= r.dirtyLo;
}

static bool g716RetireTarget(int ti, int cause);
static std::vector<uint8_t> g716ShadowMemory(const G144BlockRange &);
static void g716MaterializeForRanges(const G144BlockRange &, const G144BlockRange &,
                                     bool, bool, int);
static void g716PrepareCpuBatch();
static void g716MirrorRawToGuest();
static uint64_t g716TryTexture(const G144Entry &, const G178EntryState &);
static void g716PrepareSeed(G178Batch &, uint32_t);
static bool g716RestoreRawPage(int, uint32_t);
static bool g716CanUpload(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, bool);
static bool g716CanL2l(uint32_t, uint8_t, uint8_t, uint32_t, uint32_t,
                       uint32_t, uint8_t, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t);
static bool g716ExecuteL2l(uint8_t *, uint32_t, uint32_t, uint8_t, uint8_t, uint32_t, uint32_t,
                           uint32_t, uint8_t, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           bool, bool);
