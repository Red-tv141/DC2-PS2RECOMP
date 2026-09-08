// ===== G708: PERSISTENT PER-PAGE CONTENT DIGESTS ==============================================
//
// THE DEFECT. `g178HashPages` is the texture pass's second validity tier. The first tier compares
// a SUM OF PER-PAGE WRITE GENERATIONS (`g178GenSumRect`); when that sum has moved the runtime
// cannot conclude anything about the bytes, so it re-reads the texture's whole conservative page
// set out of guest VRAM and FNV-hashes it, purely to discover that DC2 re-uploaded the identical
// bytes (which it does every frame - "transfer is cheaper than space" - and which is exactly why
// this tier exists and why it hits).
//
// `[G511:texsplit]` counts that at **0.69 hashes per flush x 406.9 KiB = 16.6 MB/f**. G513 already
// established the loop is DEPENDENCY-bound at ~1 cycle per 8 bytes with the 8-lane body, so 16.6
// MB/f is ~2.1 M cycles/f of pure revalidation. That figure is a COUNTED QUANTITY (hashes x bytes),
// not a HOT_DIAG timer reading, so Rule 79 does not retire it.
//
// THE OBSERVATION THAT MAKES IT REMOVABLE. `g178HashPages` does not hash the texture's own texels.
// It builds `mark[]`, the set of 8 KiB GS PAGES the texture's conservative range covers, and hashes
// each marked page's FULL 8192 bytes. The quantity it computes is therefore already
//
//      f( { (page, page's 8 KiB) : page in mark } )
//
// and nothing else. Two textures that cover a page hash that page's bytes twice; the same texture
// visited on two flushes hashes them twice again. VRAM is only `kG178PageCount` = 512 pages = 4 MB,
// so 16.6 MB/f is the SAME <=4 MB re-hashed about four times over.
//
// THE MECHANISM. Give each page a persistent 64-bit digest of its 8 KiB, stamped with the page's
// own `g_g178PageGen` value at the moment it was taken. A texture's content key becomes an FNV mix
// over its marked pages of `(pageIndex, digest[page])` in ascending page order. A page is re-read
// and re-hashed if and only if its write generation has moved since its digest was taken.
//
// This is the brief's model exactly: the invalidation edge is the EXISTING write generation, which
// every VRAM writer in the runtime already bumps through the single choke point
// `g178BumpRectImpl`. No new invalidation edge is introduced, so none can be forgotten - a page
// whose bytes can have changed has, by construction, had its generation bumped, and its digest is
// then recomputed rather than trusted.
//
// EXACTNESS.
//   * SAME BYTES. The page set, the page order and the per-page byte range are byte-for-byte the
//     ones `g513HashMarkedRef`/`Fast` walk, including the `off + 8192 > vramSize` skip. The value
//     differs (it is a different digest of the same bytes, as G513's 8-lane body already is), so it
//     carries its own `hashVar` and is never compared against a stamp another body wrote.
//   * NO STALE DIGEST. A cached digest is returned only when `g_g178PageGen[pg]` still equals the
//     generation the digest was taken at. It is STORED only when the generation is unchanged across
//     the hash itself (read gen, hash, re-read gen), so a bump racing the read cannot install a
//     digest of half-written bytes under the new generation. The pre-existing bodies read the same
//     VRAM under the same conditions with no such re-check, so this is strictly safer than what it
//     replaces.
//   * WORST CASE IS PARITY. If every marked page's generation moved, every page is re-hashed and
//     the same bytes are read as before, plus one array probe per page. There is no input on which
//     this does materially more work.
//
// FLAGS
//   DC2_G708_NO_PAGEDIGEST=1  rollback to the G513 whole-set body (the A arm of the gate)
//   DC2_G708_STAT=1           [G708:pagedig] pages hashed vs reused, and bytes/frame
//   DC2_G708_VERIFY=1         oracle: recompute on every cache HIT and compare. NEVER in a timed
//                             run - it does strictly more work than either arm.
// ==============================================================================================

static const bool s_g708NoPageDigest = envFlagEnabled("DC2_G708_NO_PAGEDIGEST");
static const bool s_g708Stat = envFlagEnabled("DC2_G708_STAT");
static const bool s_g708Verify = envFlagEnabled("DC2_G708_VERIFY");

static DC2_G674_HOT_FLAG_INLINE bool g708PageDigestOn() { return !s_g708NoPageDigest; }

// The digest body's identity, so a stamp written by one body is never compared against a value
// produced by another (the G513 hashVar contract, extended by one value).
static inline uint8_t g708HashVar(bool g513Fast)
{
    return g708PageDigestOn() ? 2u : static_cast<uint8_t>(g513Fast ? 1u : 0u);
}

// Single-consumer state: `g178HashPages` has exactly one shipped call site, the texture pass of
// `g178TryFlushGpu`, which is the GS front-end thread. The generation it validates against is
// atomic because WRITERS can be other threads.
static uint64_t g_g708Digest[kG178PageCount];
static uint64_t g_g708DigestGen[kG178PageCount];
static uint8_t g_g708DigestValid[kG178PageCount];

static std::atomic<uint64_t> g_g708PagesHashed{0}, g_g708PagesReused{0}, g_g708Calls{0},
    g_g708VerifyN{0}, g_g708VerifyBad{0};

// ⭐ THE CEILING COUNTER FOR THE WRITE-SIDE FIX. `g_g708PagesRewritten` counts pages whose write
// generation MOVED since the digest was taken (so the cache could not be used and the 8 KiB had to
// be re-read) and whose recomputed digest then turned out to be IDENTICAL. Those pages were
// rewritten with the bytes they already held: they are the exact population a compare-before-write
// in the VRAM writers would stop bumping, and therefore the exact population whose re-read this
// tier is paying for and could avoid. `g_g708PagesChanged` is the complement - a real mutation, and
// irreducible. `g_g708BumpSrc` splits the identical-rewrite population by the LAST publisher of the
// page (`g_g570LastBump`, armed by DC2_G570_BUMP_TRACE=1), which names the writer to fix.
static std::atomic<uint64_t> g_g708PagesRewritten{0}, g_g708PagesChanged{0};
static std::atomic<uint64_t> g_g708BumpSrc[32];

// One page, 8 lanes, folded in a fixed lane order. Same structure as g513HashMarkedFast's inner
// loop; 8192 is an exact multiple of 64 so there is no tail.
static uint64_t g708HashOnePage(const uint8_t *p)
{
    constexpr uint64_t kP = 1099511628211ull;
    constexpr uint64_t kBasis = 1469598103934665603ull;
    constexpr uint64_t kLaneSalt = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = kBasis ^ (0ull * kLaneSalt), h1 = kBasis ^ (1ull * kLaneSalt);
    uint64_t h2 = kBasis ^ (2ull * kLaneSalt), h3 = kBasis ^ (3ull * kLaneSalt);
    uint64_t h4 = kBasis ^ (4ull * kLaneSalt), h5 = kBasis ^ (5ull * kLaneSalt);
    uint64_t h6 = kBasis ^ (6ull * kLaneSalt), h7 = kBasis ^ (7ull * kLaneSalt);
    for (size_t i = 0; i < 8192u; i += 64)
    {
        uint64_t v0, v1, v2, v3, v4, v5, v6, v7;
        std::memcpy(&v0, p + i + 0, 8);
        std::memcpy(&v1, p + i + 8, 8);
        std::memcpy(&v2, p + i + 16, 8);
        std::memcpy(&v3, p + i + 24, 8);
        std::memcpy(&v4, p + i + 32, 8);
        std::memcpy(&v5, p + i + 40, 8);
        std::memcpy(&v6, p + i + 48, 8);
        std::memcpy(&v7, p + i + 56, 8);
        h0 = (h0 ^ v0) * kP;
        h1 = (h1 ^ v1) * kP;
        h2 = (h2 ^ v2) * kP;
        h3 = (h3 ^ v3) * kP;
        h4 = (h4 ^ v4) * kP;
        h5 = (h5 ^ v5) * kP;
        h6 = (h6 ^ v6) * kP;
        h7 = (h7 ^ v7) * kP;
    }
    uint64_t hsh = kBasis;
    hsh = (hsh ^ h0) * kP;
    hsh = (hsh ^ h1) * kP;
    hsh = (hsh ^ h2) * kP;
    hsh = (hsh ^ h3) * kP;
    hsh = (hsh ^ h4) * kP;
    hsh = (hsh ^ h5) * kP;
    hsh = (hsh ^ h6) * kP;
    hsh = (hsh ^ h7) * kP;
    return hsh;
}

static void g708StatReport()
{
    const uint64_t hashed = g_g708PagesHashed.load(std::memory_order_relaxed);
    const uint64_t reused = g_g708PagesReused.load(std::memory_order_relaxed);
    const uint64_t calls = g_g708Calls.load(std::memory_order_relaxed);
    const double tot = static_cast<double>(hashed + reused);
    std::fprintf(stderr,
                 "[G708:pagedig] calls=%llu pages=%llu hashed=%llu (%.1f%%) reused=%llu (%.1f%%) "
                 "hashedKiB=%llu savedKiB=%llu verify=%llu bad=%llu\n",
                 static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(hashed + reused),
                 static_cast<unsigned long long>(hashed),
                 tot > 0.0 ? 100.0 * static_cast<double>(hashed) / tot : 0.0,
                 static_cast<unsigned long long>(reused),
                 tot > 0.0 ? 100.0 * static_cast<double>(reused) / tot : 0.0,
                 static_cast<unsigned long long>(hashed * 8ull),
                 static_cast<unsigned long long>(reused * 8ull),
                 static_cast<unsigned long long>(g_g708VerifyN.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g708VerifyBad.load(std::memory_order_relaxed)));
    const uint64_t same = g_g708PagesRewritten.load(std::memory_order_relaxed);
    const uint64_t diff = g_g708PagesChanged.load(std::memory_order_relaxed);
    const double rw = static_cast<double>(same + diff);
    std::fprintf(stderr,
                 "[G708:rewrite] rehashed=%llu identical=%llu (%.1f%%) changed=%llu (%.1f%%) "
                 "identicalKiB=%llu\n",
                 static_cast<unsigned long long>(same + diff),
                 static_cast<unsigned long long>(same),
                 rw > 0.0 ? 100.0 * static_cast<double>(same) / rw : 0.0,
                 static_cast<unsigned long long>(diff),
                 rw > 0.0 ? 100.0 * static_cast<double>(diff) / rw : 0.0,
                 static_cast<unsigned long long>(same * 8ull));
    if (same != 0u)
    {
        char line[512];
        int off2 = std::snprintf(line, sizeof(line), "[G708:bumpsrc]");
        for (uint32_t s = 0; s < 32u && off2 < static_cast<int>(sizeof(line)) - 1; ++s)
        {
            const uint64_t c = g_g708BumpSrc[s].load(std::memory_order_relaxed);
            if (c != 0u)
                off2 += std::snprintf(line + off2, sizeof(line) - off2, " %s=%.1f%%",
                                      kG582BumpName[s], 100.0 * static_cast<double>(c) /
                                                            static_cast<double>(same));
        }
        std::fprintf(stderr, "%s\n", line);
    }
    std::fflush(stderr);
}

// The persistent tier. `bytes` accumulates only what was ACTUALLY read this call, so the existing
// `[G513]` bytes census keeps meaning exactly what it meant and doubles as the armed/exercised
// proof for this lever.
static uint64_t g708HashMarkedCached(const uint8_t *vram, uint32_t vramSize, const bool *mark,
                                     uint64_t *bytesOut)
{
    constexpr uint64_t kP = 1099511628211ull;
    constexpr uint64_t kBasis = 1469598103934665603ull;
    uint64_t hsh = kBasis;
    uint64_t bytes = 0u;
    uint64_t hashedPages = 0u, reusedPages = 0u;
    for (uint32_t pg = 0; pg < kG178PageCount; ++pg)
    {
        if (!mark[pg])
            continue;
        const size_t off = static_cast<size_t>(pg) * 8192u;
        if (off + 8192u > vramSize)
            continue; // identical skip rule to g513HashMarkedRef/Fast
        const uint64_t gen0 = g_g178PageGen[pg].load(std::memory_order_relaxed);
        uint64_t digest;
        if (g_g708DigestValid[pg] != 0u && g_g708DigestGen[pg] == gen0)
        {
            digest = g_g708Digest[pg];
            ++reusedPages;
            if (s_g708Verify)
            {
                const uint64_t truth = g708HashOnePage(vram + off);
                g_g708VerifyN.fetch_add(1u, std::memory_order_relaxed);
                if (truth != digest)
                {
                    const uint64_t bad =
                        g_g708VerifyBad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (bad <= 32u)
                        std::fprintf(stderr,
                                     "[G708:pagedig-BAD] page=%u gen=%llu cached=%llx truth=%llx\n",
                                     pg, static_cast<unsigned long long>(gen0),
                                     static_cast<unsigned long long>(digest),
                                     static_cast<unsigned long long>(truth));
                }
            }
        }
        else
        {
            digest = g708HashOnePage(vram + off);
            bytes += 8192u;
            ++hashedPages;
            if (s_g708Stat && g_g708DigestValid[pg] != 0u)
            {
                if (digest == g_g708Digest[pg])
                {
                    g_g708PagesRewritten.fetch_add(1u, std::memory_order_relaxed);
                    const uint32_t src = g_g570LastBump[pg].source & 0x1fu;
                    g_g708BumpSrc[src].fetch_add(1u, std::memory_order_relaxed);
                }
                else
                    g_g708PagesChanged.fetch_add(1u, std::memory_order_relaxed);
            }
            // Install only if no writer bumped this page across the read: a bump that straddles the
            // hash would otherwise stamp a half-written image under the NEW generation and stay
            // resident. Generations are monotonic, so equality here means no bump occurred.
            if (g_g178PageGen[pg].load(std::memory_order_relaxed) == gen0)
            {
                g_g708Digest[pg] = digest;
                g_g708DigestGen[pg] = gen0;
                g_g708DigestValid[pg] = 1u;
            }
        }
        hsh = (hsh ^ static_cast<uint64_t>(pg)) * kP;
        hsh = (hsh ^ digest) * kP;
    }
    if (bytesOut)
        *bytesOut = bytes;
    if (s_g708Stat)
    {
        g_g708PagesHashed.fetch_add(hashedPages, std::memory_order_relaxed);
        g_g708PagesReused.fetch_add(reusedPages, std::memory_order_relaxed);
        if ((g_g708Calls.fetch_add(1u, std::memory_order_relaxed) % 2000ull) == 0ull)
            g708StatReport();
    }
    return hsh;
}
