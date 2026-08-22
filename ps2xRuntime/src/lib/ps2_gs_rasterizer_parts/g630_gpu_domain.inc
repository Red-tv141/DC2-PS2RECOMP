// ===== G630: PERSISTENT GPU-AUTHORITATIVE RAW VRAM DOMAIN =====================================
// One 4 MiB R32UI image is the authority for admitted G629 writes. Physical aliases therefore
// remain aliases. Guest VRAM is refreshed only before a proven CPU reader/writer; CT32 GPU
// consumers receive an ordered raw->FBO view and never force publication.
// Arm DC2_G630_RESIDENT=1. Rollback DC2_G630_NO_RESIDENT=1. Census DC2_G630_STAT=1.

static constexpr uint32_t kG630Pages = 512u;
static constexpr uint32_t kG630WordsPerPage = 2048u;

struct G630Surface
{
    uint32_t fbp = 0u, block = 0u, fbw = 0u;
    int fbW = 0, covLo = 512, covHi = -1;
    int viewLo = 512, viewHi = -1;
    uint64_t seq = 0u, viewSeq = 0u, viewGen = 0u, viewGlobalGen = 0u;
    // G633 event generations replace per-draw guest-page scans in the compiled arm.
    uint64_t generation = 1u, viewGeneration = 0u, shapeGeneration = 1u;
    std::array<uint64_t, 8> pageBits{}, residentBits{};
    bool valid = false;
};

struct G630Pending
{
    std::shared_ptr<G630ReplayPacket> packet;
    std::shared_ptr<G630UploadPacket> upload;
    std::shared_ptr<G630T8Packet> t8;
    std::vector<G144Entry> fallback;
    GS *gs = nullptr;
    bool view = false;
};

static std::array<uint8_t, kG630Pages> g_g630GpuDirty{};
static std::array<uint64_t, kG630Pages> g_g630SyncedGen{};
static std::unordered_map<uint32_t, G630Surface> g_g630Surfaces;
static std::deque<G630Pending> g_g630Pending;
static std::deque<std::shared_ptr<G630ReplayPacket>> g_g630RecyclePackets;
static std::unordered_set<uint64_t> g_g630T8Built;
static uint8_t *g_g630Vram = nullptr;
static uint32_t g_g630VramSize = 0u;
static uint64_t g_g630Seq = 0u;
static bool g_g630Initialized = false;
static bool g_g630DepthValid = false, g_g630DepthDirty = false;
static bool g_g630ForceSync = false, g_g630Fault = false;

static std::atomic<uint64_t> g_g630Submitted{0u}, g_g630Views{0u}, g_g630Retired{0u};
static std::atomic<uint64_t> g_g630Failed{0u}, g_g630Materialize{0u}, g_g630MatPages{0u};
static std::atomic<uint64_t> g_g630SeedBytes{0u}, g_g630PatchBytes{0u};
static std::atomic<uint64_t> g_g630DirectBinds{0u}, g_g630CpuBreaks{0u};
static std::atomic<uint64_t> g_g630Uploads{0u}, g_g630UploadBytes{0u};
static std::atomic<uint64_t> g_g630T8Views{0u}, g_g630T8Pixels{0u};
static std::array<std::atomic<uint64_t>, 9> g_g630MatByCause{};
static std::array<std::atomic<uint64_t>, 9> g_g630PagesByCause{};
static std::array<std::atomic<uint64_t>, 9> g_g630NsByCause{};
static std::atomic<uint64_t> g_g630UnknownBreaks{0u};
static constexpr int kG630MatGpuInput = 8;

// G636's arbitration between this domain and the native renderer's G261 FBO residency is included
// below (it needs g630AppendGuestPages), but g630Report() above it has to be able to print.
static void g636Report();

// G634's raw CT32/CT24 texture view is defined after every G630 helper it reuses.
static void g634BeginTexturePass();
static void g634Report();
static bool g634CanIngestUpload(uint32_t block, uint32_t fbw, uint32_t psm,
                                uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                bool unknown);
static bool g634IngestUpload(uint8_t *vram, uint32_t block, uint32_t fbw, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// G633 is defined after the legacy CT32/T8 predicates so its verifier can call them directly.
static bool g633PlansOn();
static void g633AuthorityChanged(uint32_t page, bool dirty);
static void g633AuthorityCleared();
static void g633Report();
static void g633ResetDomain();
static void g633CommitReplaySurface(uint32_t fbp, uint32_t fbw, int height,
                                    uint32_t firstPage, uint32_t lastPage);
static void g633NoteRawRange(const G144BlockRange &range, uint32_t mirroredFbp);
static int g633SurfaceFbW(uint32_t fbp);
static void g633BeginTexturePass();
static void g633FinalizeTexturePass();
static uint32_t g633TryBind(uint32_t dstFbp, const G144Entry &e,
                            const G178EntryState &st, bool alreadyBound, uint32_t *outMask);
static uint64_t g633TryT8View(const G144Entry &e, const G178EntryState &st, uint32_t *outMask);

// G632: coherence epochs + the reject-conjunct bitmasks for the two direct-view gates below.
// Included here (not from ps2_gs_rasterizer.cpp) because every user of it is in this file and in
// the texture bind pre-pass that this file serves.
#include "g632_coherence_epoch.inc"

static bool g630DomainOn()
{
    static const bool on = envFlagEnabled("DC2_G630_RESIDENT") &&
                           !envFlagEnabled("DC2_G630_NO_RESIDENT") &&
                           !envFlagEnabled("DC2_G629_VERIFY");
    return on && !g_g630ForceSync && !g_g630Fault;
}

static bool g630StatOn()
{
    static const bool on = envFlagEnabled("DC2_G630_STAT");
    return on;
}

// G631 raw-VRAM readback census. Defined beside the run-scoped reader below.
static void g631ReportReads();

// ===== G632: THE AUTHORITY MAP AS A 512-BIT SET ===============================================
// `[G332:gsw]` says 81% of the resident domain's deficit is FRONT-END CPU on the pole thread, not
// GPU, and `[G632:gate]` / `[G632:wait]` say the publications themselves are only ~1.9 ms/f of it.
// The rest is per-DRAW bookkeeping: `g630MaterializeForRanges` runs once per textured draw
// (11.4 M calls per `ridepod` run) and each call zero-fills a 512-byte `wanted` array and then
// scans all 512 bytes against `g_g630GpuDirty` — to answer a question that is almost always "no".
//
// `g_g630DirtyBits` mirrors `g_g630GpuDirty` bit for bit and `g_g630DirtyCount` counts its set
// bits, both maintained at the only three places authority changes hands (submit sets, publish
// clears, recovery clears). The hot test becomes: build the wanted RANGE as bits (a couple of
// word writes), AND it against 8 words, and only materialise the byte array on the rare hit.
// Exact by construction — the bitset is a redundant encoding of the same array, and
// DC2_G632_NO_FASTDIRTY=1 restores the original scans verbatim.
static std::array<uint64_t, 8> g_g630DirtyBits{};
static uint32_t g_g630DirtyCount = 0u;

// PROMOTION STATE: default-OFF, like every other lever in this phase, because it has not been
// through a paired gate. `DC2_G632_FASTDIRTY=1` arms it; `DC2_G632_NO_FASTDIRTY=1` is the rollback
// that must keep working after promotion. The bitset itself is maintained UNCONDITIONALLY so the
// two arms differ only in which encoding is READ — an arm that also changed which one is WRITTEN
// would not be a rollback of one variable.
// G633 promotion gate: -3.585 ms/f (-11.61%), with both A/B order blocks agreeing within
// 0.013 ms/f. The byte map remains maintained so NO_FASTDIRTY is still an exact rollback.
static constexpr bool kG632FastDirtyDefaultOn = true;
static const bool s_g632FastDirty = !envFlagEnabled("DC2_G632_NO_FASTDIRTY") &&
                                    (kG632FastDirtyDefaultOn ||
                                     envFlagEnabled("DC2_G632_FASTDIRTY"));

static inline void g630SetDirty(uint32_t p)
{
    if (g_g630GpuDirty[p] != 0u) return;
    g_g630GpuDirty[p] = 1u;
    g_g630DirtyBits[p >> 6] |= 1ull << (p & 63u);
    ++g_g630DirtyCount;
    g633AuthorityChanged(p, true);
}

static inline void g630ClearDirty(uint32_t p)
{
    if (g_g630GpuDirty[p] == 0u) return;
    g_g630GpuDirty[p] = 0u;
    g_g630DirtyBits[p >> 6] &= ~(1ull << (p & 63u));
    --g_g630DirtyCount;
    g633AuthorityChanged(p, false);
}

static inline void g630ClearAllDirty()
{
    g_g630GpuDirty.fill(0u);
    g_g630DirtyBits.fill(0ull);
    g_g630DirtyCount = 0u;
    g633AuthorityCleared();
}

static bool g630AnyDirty()
{
    if (s_g632FastDirty) return g_g630DirtyCount != 0u;
    for (uint8_t v : g_g630GpuDirty)
        if (v != 0u) return true;
    return false;
}

static void g630Report()
{
    // G632's census is armed independently of DC2_G630_STAT, so it must not ride on that gate.
    if (!g630StatOn()) { g632Report(); g633Report(); g634Report(); g636Report(); return; }
    std::fprintf(stderr,
                 "[G630:domain] submit=%llu retire=%llu fail=%llu views=%llu t8=%llu/%.1fMpx "
                 "binds=%llu "
                 "seed=%.1fMiB patch=%.1fMiB up=%llu/%.1fMiB | mat=%llu pages=%llu "
                 "cpuBreak=%llu pending=%zu\n",
                 (unsigned long long)g_g630Submitted.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630Retired.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630Failed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630Views.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630T8Views.load(std::memory_order_relaxed),
                 (double)g_g630T8Pixels.load(std::memory_order_relaxed) / 1000000.0,
                 (unsigned long long)g_g630DirectBinds.load(std::memory_order_relaxed),
                 (double)g_g630SeedBytes.load(std::memory_order_relaxed) / 1048576.0,
                 (double)g_g630PatchBytes.load(std::memory_order_relaxed) / 1048576.0,
                 (unsigned long long)g_g630Uploads.load(std::memory_order_relaxed),
                 (double)g_g630UploadBytes.load(std::memory_order_relaxed) / 1048576.0,
                 (unsigned long long)g_g630Materialize.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630MatPages.load(std::memory_order_relaxed),
                 (unsigned long long)g_g630CpuBreaks.load(std::memory_order_relaxed),
                 g_g630Pending.size());
    std::fprintf(stderr,
                 "[G630:break] cpu=%llu/%llu/%.1f inl=%llu/%llu/%.1f up=%llu/%llu/%.1f "
                 "local=%llu/%llu/%.1f l2h=%llu/%llu/%.1f tex=%llu/%llu/%.1f "
                 "gpuin=%llu/%llu/%.1f unknown=%llu\n",
                 (unsigned long long)g_g630MatByCause[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[0].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[0].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[1].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[1].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[2].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[2].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[3].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[3].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[4].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[4].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[5].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[5].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630MatByCause[8].load(std::memory_order_relaxed),
                 (unsigned long long)g_g630PagesByCause[8].load(std::memory_order_relaxed),
                 (double)g_g630NsByCause[8].load(std::memory_order_relaxed) / 1e6,
                 (unsigned long long)g_g630UnknownBreaks.load(std::memory_order_relaxed));
    g631ReportReads();
    g632Report();
    g633Report();
    g634Report();
    g636Report();
}

static std::shared_ptr<G630ReplayPacket> g630TakeReplayPacket()
{
    if (g_g630RecyclePackets.empty())
        return std::make_shared<G630ReplayPacket>();
    auto p = std::move(g_g630RecyclePackets.front());
    g_g630RecyclePackets.pop_front();
    p->recs.clear();
    p->texDesc.clear();
    p->texData.clear();
    p->tileOff.clear();
    p->tileRef.clear();
    p->seedVram.clear();
    p->patchPages.clear();
    p->patchWords.clear();
    p->depthSeed.clear();
    return p;
}

static void g630RecoverFailure(G630Pending &p)
{
    g_g630Failed.fetch_add(1u, std::memory_order_relaxed);
    // Fail closed: publish every authority that completed before the failed FIFO item. An upload
    // packet is then replayed over that shadow because its guest write happened after the raw
    // producer and must win even when the GPU patch itself failed.
    if (g_g630Initialized && g_g630Vram != nullptr)
    {
        std::vector<uint32_t> raw;
        if (g630_backend_read_vram(raw) && raw.size() >= 1024u * 1024u)
        {
            bool any = false;
            for (uint32_t page = 0u; page < kG630Pages; ++page)
            {
                if (g_g630GpuDirty[page] == 0u) continue;
                std::memcpy(g_g630Vram + static_cast<size_t>(page) * 8192u,
                            raw.data() + static_cast<size_t>(page) * kG630WordsPerPage, 8192u);
                g630ClearDirty(page);
                g_g178PageGen[page].fetch_add(1u, std::memory_order_relaxed);
                any = true;
            }
            if (any) g149BumpVramGen();
        }
    }
    if (g_g630DepthDirty)
    {
        std::vector<uint32_t> z;
        if (g630_backend_read_depth(z) && z.size() == g403DisplayZBuf().size())
            std::copy(z.begin(), z.end(), g403DisplayZBuf().begin());
    }
    if (p.upload && g_g630Vram != nullptr)
    {
        const G630UploadPacket &u = *p.upload;
        size_t n = 0u;
        for (int y = 0; y < u.height; ++y)
            for (int x = 0; x < u.width; ++x)
                GSMem::WriteCT32(g_g630Vram, u.block, static_cast<uint32_t>(u.fbw),
                                 static_cast<uint32_t>(u.x + x),
                                 static_cast<uint32_t>(u.y + y), u.pixels[n++]);
        const G144BlockRange r = g144RangeForRect(
            u.block, GS_PSM_CT32, static_cast<uint32_t>(u.fbw),
            static_cast<uint32_t>(u.x), static_cast<uint32_t>(u.y),
            static_cast<uint32_t>(u.width), static_cast<uint32_t>(u.height));
        if (r.valid)
            for (uint32_t page = r.first >> 5u;
                 page <= std::min(kG630Pages - 1u, r.last >> 5u); ++page)
                g_g178PageGen[page].fetch_add(1u, std::memory_order_relaxed);
        g149BumpVramGen();
    }
    g_g630Fault = true;
    g_g630Initialized = false;
    g_g630DepthValid = false;
    g_g630DepthDirty = false;
    g630ClearAllDirty();
    g_g630Surfaces.clear();
    g633ResetDomain();
    if (p.fallback.empty() || p.gs == nullptr || !g_g144FlushSeqFn)
        return;
    std::vector<G144Entry> saved;
    saved.swap(g_g144List);
    GS *savedGs = g_g144LastGs;
    g_g144List.swap(p.fallback);
    g_g144LastGs = p.gs;
    g_g630ForceSync = true;
    g_g144FlushSeqFn();
    g_g630ForceSync = false;
    g_g144List.swap(saved);
    g_g144LastGs = savedGs;
}

static void g630Poll(bool wait)
{
    if (g_g630Pending.empty()) return;
    std::vector<uint8_t> results;
    if (!g630_backend_poll(wait, results)) return;
    for (uint8_t ok : results)
    {
        if (g_g630Pending.empty()) break;
        G630Pending p = std::move(g_g630Pending.front());
        g_g630Pending.pop_front();
        if (ok == 0u) g630RecoverFailure(p);
        else if (p.packet) g_g630RecyclePackets.push_back(std::move(p.packet));
        g_g630Retired.fetch_add(1u, std::memory_order_relaxed);
    }
}

static void g630AppendGuestPages(uint32_t first, uint32_t count,
                                 std::vector<uint32_t> &pages,
                                 std::vector<uint32_t> &words)
{
    if (g_g630Vram == nullptr || g_g630VramSize < 4u * 1024u * 1024u) return;
    const uint32_t last = std::min(kG630Pages, first + count);
    for (uint32_t p = first; p < last; ++p)
    {
        const uint64_t gen = g_g178PageGen[p].load(std::memory_order_relaxed);
        if (g_g630Initialized && g_g630GpuDirty[p] == 0u && g_g630SyncedGen[p] == gen)
            continue;
        if (g_g630GpuDirty[p] != 0u) continue;
        pages.push_back(p);
        const uint32_t *src = reinterpret_cast<const uint32_t *>(g_g630Vram) +
                              static_cast<size_t>(p) * kG630WordsPerPage;
        words.insert(words.end(), src, src + kG630WordsPerPage);
    }
}

static void g630CommitSyncedPages(const std::vector<uint32_t> &pages)
{
    for (uint32_t p : pages)
        if (p < kG630Pages)
            g_g630SyncedGen[p] = g_g178PageGen[p].load(std::memory_order_relaxed);
    g_g630PatchBytes.fetch_add(pages.size() * 8192ull, std::memory_order_relaxed);
}

// G636: the arbitration predicate between this raw-image authority and the native renderer's G261
// FBO residency, plus the row-band page helpers the CT32 view needs. Placed here because it uses
// g630AppendGuestPages above and every consumer of it is below.
#include "g636_authority_arbitration.inc"

// Defined below; the G636 SYNCPUB bisect arms at the end of g630SubmitReplay need them.
static bool g630ReadAndPublish(const std::array<uint8_t, kG630Pages> &wanted, int cause);
static void g630MaterializeDepth();

// ===== G636: THE DEPTH MIRROR'S MISSING INVALIDATION ==========================================
// `g_g630DepthShadow` is a copy of `g403DisplayZBuf()` as of the last instant the GPU mirror and it
// were the same depth. If the CPU buffer has moved away from it, some writer outside the CPU band
// replay changed depth and the mirror must be re-seeded. Exact by construction: the GPU mirror only
// ever changes through a resident replay (which leaves the CPU buffer untouched, so the shadow
// still matches and no re-seed is forced) or through a publication (which re-syncs the shadow).
static std::vector<uint32_t> g_g630DepthShadow;

static bool g636ZBufChanged()
{
    const std::vector<uint32_t> &z = g403DisplayZBuf();
    if (g_g630DepthShadow.size() != z.size()) return true;
    if (std::memcmp(g_g630DepthShadow.data(), z.data(), z.size() * sizeof(uint32_t)) == 0)
        return false;
    if (s_g636Stat)
    {
        // Name the writer by its FOOTPRINT: a whole-surface clear, a scissor band, or a few spans.
        static uint32_t s_prints = 0u;
        if (s_prints < 8u)
        {
            ++s_prints;
            uint64_t diff = 0u, toZero = 0u, fromZero = 0u;
            int lo = 512, hi = -1;
            for (size_t i = 0; i < z.size(); ++i)
            {
                if (z[i] == g_g630DepthShadow[i]) continue;
                ++diff;
                if (z[i] == 0u) ++toZero;
                if (g_g630DepthShadow[i] == 0u) ++fromZero;
                const int row = static_cast<int>(i / kG403DisplayZW);
                if (row < lo) lo = row;
                if (row > hi) hi = row;
            }
            std::fprintf(stderr,
                         "[G636:zwho] changed=%llu/%zu rows=%d..%d ->zero=%llu fromZero=%llu "
                         "dirty=%u\n",
                         (unsigned long long)diff, z.size(), lo, hi,
                         (unsigned long long)toZero, (unsigned long long)fromZero,
                         g_g630DepthDirty ? 1u : 0u);
        }
    }
    return true;
}

static void g636ZShadowSync()
{
    const std::vector<uint32_t> &z = g403DisplayZBuf();
    g_g630DepthShadow.assign(z.begin(), z.end());
    g_g636ZSeeds.fetch_add(1u, std::memory_order_relaxed);
}

// ===== G636: THE RAW-IMAGE FAITHFULNESS ORACLE ================================================
// G630's premise is "row p of the raw image IS guest page p". For a page the GPU does NOT own that
// is a CLAIM, maintained by g630AppendGuestPages / g_g630SyncedGen — and nothing ever checked it.
// Read the image back and compare exactly the pages whose authority is guest VRAM. That is the
// population that CAN fail (G635 law 2); the GPU-owned pages are excluded because there the raw
// image is the authority by definition.
static uint64_t g_g636VfyPages = 0u, g_g636VfyBad = 0u, g_g636VfyCalls = 0u;
static std::array<uint32_t, 32> g_g636VfyBySource{};

static void g636VerifyRawImage()
{
    if (!s_g636RawVfy || !g_g630Initialized || g_g630Vram == nullptr) return;
    std::vector<uint32_t> raw;
    if (!g630_backend_read_vram(raw) ||
        raw.size() < static_cast<size_t>(kG630Pages) * kG630WordsPerPage)
        return;
    ++g_g636VfyCalls;
    const uint32_t *guest = reinterpret_cast<const uint32_t *>(g_g630Vram);
    for (uint32_t p = 0u; p < kG630Pages; ++p)
    {
        if (g_g630GpuDirty[p] != 0u) continue; // GPU owns it: the image IS the authority
        // The CLAIM is `syncedGen == pageGen` — that is the only state in which the domain will
        // skip a patch. A page whose generation has moved is simply not claimed and will be
        // patched on demand, so including it would test a proposition the domain never asserted.
        if (g_g630SyncedGen[p] != g_g178PageGen[p].load(std::memory_order_relaxed)) continue;
        ++g_g636VfyPages;
        const size_t off = static_cast<size_t>(p) * kG630WordsPerPage;
        if (std::memcmp(raw.data() + off, guest + off, kG630WordsPerPage * 4u) == 0) continue;
        ++g_g636VfyBad;
        const uint32_t src = g_g570LastBump[p & (kG178PageCount - 1u)].source;
        ++g_g636VfyBySource[std::min(src & 0xffu, 31u)];
        if (g_g636VfyBad <= 12u)
            std::fprintf(stderr,
                         "[G636:rawvfy] STALE page=%03x lastBumpSrc=%u gen=%llu synced=%llu\n",
                         p, src,
                         (unsigned long long)g_g178PageGen[p].load(std::memory_order_relaxed),
                         (unsigned long long)g_g630SyncedGen[p]);
    }
    if ((g_g636VfyCalls % 128u) == 0u)
    {
        std::fprintf(stderr, "[G636:rawvfy] calls=%llu checked=%llu bad=%llu bySrc:",
                     (unsigned long long)g_g636VfyCalls,
                     (unsigned long long)g_g636VfyPages,
                     (unsigned long long)g_g636VfyBad);
        for (uint32_t s = 0u; s < 32u; ++s)
            if (g_g636VfyBySource[s] != 0u)
                std::fprintf(stderr, " %u=%u", s, g_g636VfyBySource[s]);
        std::fprintf(stderr, "\n");
    }
}

static bool g630SubmitReplay(const std::shared_ptr<G630ReplayPacket> &packet,
                             std::vector<G144Entry> &list, GS *gs,
                             uint8_t *vram, uint32_t vramSize,
                             uint32_t firstPage, uint32_t pageCount,
                             uint32_t fbp, uint32_t fbw, int height)
{
    if (!g630DomainOn() || !packet || gs == nullptr || vram == nullptr ||
        vramSize < 4u * 1024u * 1024u)
        return false;
    g630Poll(false);
    if (g_g630Fault) return false;
    g_g630Vram = vram;
    g_g630VramSize = vramSize;
    g636VerifyRawImage(); // G636 oracle: is the image still a faithful mirror of guest VRAM?
    if (!g_g630Initialized)
    {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(g_g630Vram);
        packet->seedVram.assign(src, src + (4u * 1024u * 1024u / 4u));
    }
    else
        g630AppendGuestPages(firstPage, pageCount, packet->patchPages, packet->patchWords);
    // ⭐⭐⭐ G636 DEFECT A: decide whether the persistent depth mirror is still the newest depth.
    if (packet->wantDepth && g_g630DepthValid)
    {
        if (s_g636ZReseed) g_g630DepthValid = false;
        else if (!s_g636NoZShadow && g636ZBufChanged())
        {
            // Someone wrote the CPU Z buffer since the mirror was last synchronised with it, and it
            // was not a CPU band replay (that path calls g630NoteCpuReplay). The mirror is behind.
            g_g630DepthValid = false;
            g_g636ZStale.fetch_add(1u, std::memory_order_relaxed);
            if (g_g630DepthDirty)
                g_g636ZLost.fetch_add(1u, std::memory_order_relaxed);
        }
    }
    if (packet->wantDepth && !g_g630DepthValid)
    {
        packet->depthSeed.assign(g403DisplayZBuf().begin(), g403DisplayZBuf().end());
        g636ZShadowSync();
    }
    if (!g630_backend_submit(packet)) return false;

    if (!g_g630Initialized)
    {
        for (uint32_t p = 0; p < kG630Pages; ++p)
            g_g630SyncedGen[p] = g_g178PageGen[p].load(std::memory_order_relaxed);
        g_g630Initialized = true;
        g_g630SeedBytes.fetch_add(4u * 1024u * 1024u, std::memory_order_relaxed);
    }
    g630CommitSyncedPages(packet->patchPages);
    const uint32_t last = std::min(kG630Pages, firstPage + pageCount);
    for (uint32_t p = firstPage; p < last; ++p) g630SetDirty(p);
    if (packet->wantDepth) { g_g630DepthValid = true; g_g630DepthDirty = true; }
    if (g633PlansOn())
        g633CommitReplaySurface(fbp, fbw, height, firstPage, last == 0u ? 0u : last - 1u);
    else
    {
        G630Surface &s = g_g630Surfaces[fbp];
        s.fbp = fbp; s.block = fbp << 5u; s.fbw = fbw; s.fbW = static_cast<int>(fbw) * 64;
        s.covLo = 0; s.covHi = std::max(s.covHi, height - 1); s.seq = ++g_g630Seq; s.valid = true;
        // The exact replay dispatch writes the raw word and its linear FBO mirror together.
        s.viewSeq = s.seq;
        s.viewLo = 0;
        s.viewHi = std::max(s.viewHi, std::min(511, height - 1));
        s.viewGlobalGen = g_g149VramGen.load(std::memory_order_relaxed);
        s.viewGen = 0u;
        for (uint32_t p = firstPage; p < last; ++p)
            s.viewGen += g_g178PageGen[p].load(std::memory_order_relaxed);
    }
    G630Pending pending;
    pending.packet = packet;
    pending.gs = gs;
    pending.fallback.swap(list);
    g_g630Pending.push_back(std::move(pending));
    const uint64_t n = g_g630Submitted.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 256u) == 0u) g630Report();
    // G636 bisect: collapse residency to the non-resident G629 semantics, one authority at a time.
    if (s_g636SyncPub)
    {
        std::array<uint8_t, kG630Pages> wanted{};
        for (uint32_t p = firstPage; p < last; ++p) wanted[p] = 1u;
        (void)g630ReadAndPublish(wanted, 0);
    }
    if (s_g636SyncPubZ) g630MaterializeDepth();
    return true;
}

// G631: publish only the RUNS of pages this consumer actually needs.
//
// The legacy form read the entire 2048x512 raw image (4 MiB) for every CPU break, and `ridepod`
// takes about 6 breaks per frame — 25 MiB/f of GPU->CPU transfer to publish a few pages. Row p of
// that image IS guest page p, so a maximal run of wanted-and-dirty pages is one contiguous
// glReadPixels band. Refusals fall back to the whole-image read, which is detected by size and
// handled with the identical page loop, so this can only ever move BYTES, never which pages are
// published or what they contain.
static bool g630ReadRunOn()
{
    static const bool on = (std::getenv("DC2_G631_NO_READRUN") == nullptr);
    return on;
}

static std::atomic<uint64_t> g_g631ReadCalls{0u}, g_g631ReadRows{0u}, g_g631ReadFull{0u};

static void g631ReportReads()
{
    const uint64_t calls = g_g631ReadCalls.load(std::memory_order_relaxed);
    if (calls == 0u) return;
    const uint64_t rows = g_g631ReadRows.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G631:read] run=%u calls=%llu rows=%llu rows/call=%.1f fullFallback=%llu "
                 "MiB=%.1f (whole-image equivalent %.1f MiB)\n",
                 g630ReadRunOn() ? 1u : 0u,
                 (unsigned long long)calls, (unsigned long long)rows,
                 (double)rows / (double)calls,
                 (unsigned long long)g_g631ReadFull.load(std::memory_order_relaxed),
                 (double)rows * 8192.0 / 1048576.0,
                 (double)calls * 4.0);
}

// ===== G632: THE DOUBLE WAIT ==================================================================
// `g630Poll(true)` blocks on every in-flight G630 future, and the backend read below is then
// QUEUED BEHIND THOSE SAME ITEMS on the one shared G178 FIFO — `G178Backend::run()` drains its
// queue strictly in order, so the read's own `wait.wait()` already subsumes the poll's. Two
// blocking synchronizations are paid where the ordering only ever needed one.
//
// The poll is not redundant in PURPOSE, only in placement: it exists so a FAILED item is observed
// before guest VRAM is overwritten with content that item was supposed to have produced. That
// observation is equally valid AFTER the read, provided nothing is committed until it has been
// made. So with the lever armed the reads run first into staged bands, the poll follows (and now
// returns immediately, because the worker has already passed those items), and the guest-VRAM
// commit happens only if no item failed. On failure `g630RecoverFailure` publishes every dirty
// page itself and raises `g_g630Fault`, exactly as it does today, and the staged bands are
// discarded — so the failure path is bit-identical and only the SUCCESS path loses a wait.
static bool g630LatePollOn()
{
    static const bool on = !envFlagEnabled("DC2_G632_NO_LATEPOLL") &&
                           envFlagEnabled("DC2_G632_LATEPOLL");
    return on;
}

struct G632StagedBand
{
    uint32_t p = 0u, q = 0u, base = 0u;
    std::vector<uint32_t> data;
};

static bool g630ReadAndPublish(const std::array<uint8_t, kG630Pages> &wanted, int cause)
{
    const auto t0 = std::chrono::steady_clock::now();
    const bool latePoll = g630LatePollOn() && g630ReadRunOn();
    if (!latePoll)
    {
        // Clock the poll on its own. If it is already ~free the "double wait" does not exist and
        // no reordering of it can pay — the read's round trip is the whole cost.
        if (g632StatOn())
        {
            const auto p0 = std::chrono::steady_clock::now();
            g630Poll(true);
            g_g632PubNs[0] += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - p0).count());
            ++g_g632PubCalls[0];
        }
        else
            g630Poll(true);
    }
    if (g_g630Fault || g_g630Vram == nullptr) return false;
    uint64_t pages = 0u;
    std::vector<G632StagedBand> staged;
    if (g630ReadRunOn())
    {
        // ⭐ THE COST HERE IS LATENCY, NOT BYTES. Each backend read is a synchronous round trip
        // that queues behind everything already in the shared GL FIFO, so it prices at ~0.7-1.3 ms
        // regardless of size, while the transfer itself is 8 KiB per page. Bridging a short gap
        // between two qualifying runs therefore trades a few tens of KiB for a whole round trip.
        // Pages inside a bridged gap are READ but never published — the publish loop below still
        // tests `wanted && dirty` per page, so authority is unchanged.
        static constexpr uint32_t kG631ReadGap = 16u;
        std::vector<uint32_t> band;
        uint32_t p = 0u;
        bool any = false;
        while (p < kG630Pages)
        {
            if (wanted[p] == 0u || g_g630GpuDirty[p] == 0u) { ++p; continue; }
            uint32_t q = p + 1u;    // one past the last page to READ
            uint32_t last = p + 1u; // one past the last QUALIFYING page seen
            while (q < kG630Pages && q - last <= kG631ReadGap)
            {
                if (wanted[q] != 0u && g_g630GpuDirty[q] != 0u) last = q + 1u;
                ++q;
            }
            q = last;
            const uint32_t count = q - p;
            const auto r0 = g632StatOn() ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
            if (!g630_backend_read_vram_rows(p, count, band)) return false;
            if (g632StatOn())
            {
                g_g632PubNs[1] += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - r0).count());
                ++g_g632PubCalls[1];
            }
            // The backend falls back to the whole image when it cannot take the band path; the
            // only signal is the size, so re-base the source offset accordingly.
            const bool full = band.size() >= static_cast<size_t>(kG630Pages) * kG630WordsPerPage;
            if (!full && band.size() != static_cast<size_t>(count) * kG630WordsPerPage)
                return false;
            const uint32_t base = full ? 0u : p;
            g_g631ReadCalls.fetch_add(1u, std::memory_order_relaxed);
            g_g631ReadRows.fetch_add(full ? kG630Pages : count, std::memory_order_relaxed);
            if (full) g_g631ReadFull.fetch_add(1u, std::memory_order_relaxed);
            if (latePoll)
            {
                G632StagedBand s;
                s.p = p; s.q = q; s.base = base;
                s.data.swap(band);
                staged.push_back(std::move(s));
            }
            else
            {
                const auto c0 = g632StatOn() ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
                for (uint32_t r = p; r < q; ++r)
                {
                    if (wanted[r] == 0u || g_g630GpuDirty[r] == 0u) continue; // bridged gap page
                    std::memcpy(g_g630Vram + static_cast<size_t>(r) * 8192u,
                                band.data() + static_cast<size_t>(r - base) * kG630WordsPerPage,
                                8192u);
                    g630ClearDirty(r);
                    const uint64_t gen =
                        g_g178PageGen[r].fetch_add(1u, std::memory_order_relaxed) + 1u;
                    g_g630SyncedGen[r] = gen;
                    ++pages;
                }
                if (g632StatOn())
                {
                    g_g632PubNs[2] += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - c0).count());
                    ++g_g632PubCalls[2];
                }
            }
            any = true;
            p = q;
        }
        (void)any;
        if (latePoll)
        {
            // The observation the poll exists for, now that every read has landed. A failure has
            // already republished everything and raised the fault, so the staged bands are void.
            g630Poll(true);
            if (g_g630Fault) return false;
            for (const G632StagedBand &s : staged)
                for (uint32_t r = s.p; r < s.q; ++r)
                {
                    if (wanted[r] == 0u || g_g630GpuDirty[r] == 0u) continue;
                    std::memcpy(g_g630Vram + static_cast<size_t>(r) * 8192u,
                                s.data.data() + static_cast<size_t>(r - s.base) * kG630WordsPerPage,
                                8192u);
                    g630ClearDirty(r);
                    const uint64_t gen =
                        g_g178PageGen[r].fetch_add(1u, std::memory_order_relaxed) + 1u;
                    g_g630SyncedGen[r] = gen;
                    ++pages;
                }
        }
    }
    else
    {
    std::vector<uint32_t> raw;
    if (!g630_backend_read_vram(raw) || raw.size() < 1024u * 1024u) return false;
    for (uint32_t p = 0; p < kG630Pages; ++p)
    {
        if (wanted[p] == 0u || g_g630GpuDirty[p] == 0u) continue;
        std::memcpy(g_g630Vram + static_cast<size_t>(p) * 8192u,
                    raw.data() + static_cast<size_t>(p) * kG630WordsPerPage, 8192u);
        g630ClearDirty(p);
        const uint64_t gen = g_g178PageGen[p].fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g630SyncedGen[p] = gen;
        ++pages;
    }
    }
    if (pages != 0u)
    {
        g149BumpVramGen();
        g_g630Materialize.fetch_add(1u, std::memory_order_relaxed);
        g_g630MatPages.fetch_add(pages, std::memory_order_relaxed);
        const size_t ci = (cause >= 0 && cause < 9) ? static_cast<size_t>(cause) : 0u;
        g_g630MatByCause[ci].fetch_add(1u, std::memory_order_relaxed);
        g_g630PagesByCause[ci].fetch_add(pages, std::memory_order_relaxed);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        g_g630NsByCause[ci].fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
    }
    return true;
}

static void g630MaterializeDepth()
{
    if (!g_g630DepthDirty) return;
    g630Poll(true);
    std::vector<uint32_t> z;
    if (g630_backend_read_depth(z) && z.size() == g403DisplayZBuf().size())
    {
        std::copy(z.begin(), z.end(), g403DisplayZBuf().begin());
        g_g630DepthDirty = false;
        // G636: the two are the same depth again — that is exactly what the shadow records.
        if (!s_g636NoZShadow) g636ZShadowSync();
    }
    else
        g_g630Fault = true;
}

// ⭐⭐⭐ G636: SETTLE BOTH RESIDENT AUTHORITIES FOR A WHOLE CT32 SURFACE BAND.
//
// `g570Batch139*` reads the ENTIRE 512x512 colour band of target 0x139 straight out of guest VRAM
// and the shared depth mirror `g403DisplayZBuf()`, re-rasterises the flush's triangles onto them,
// and WRITES THE BAND BACK — and it runs on the flush path even when the resident GPU replay
// already took that flush (`if (g570GpuBatch)` sits outside the `!g629Gpu` branch). Neither
// authority was settled first, so under `DC2_G630_RESIDENT` it read pre-replay bytes and then
// committed them over the replay's own output. `[G636:zmirror]` measured the depth half of it as
// 1,080 stale detections, EVERY ONE with the GPU mirror still unpublished.
//
// This is the same law as the rest of the phase: a consumer that reads guest VRAM or the shared
// depth mirror directly must settle the resident authority for what it reads, first.
static void g636SettleSurfaceBand(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi, bool depth)
{
    if (!g630DomainOn()) return;
    if (fbw != 0u && rowHi >= rowLo && g630AnyDirty())
    {
        const G144BlockRange r = g144RangeForRect(
            fbp << 5u, GS_PSM_CT32, fbw, 0u, static_cast<uint32_t>(rowLo),
            fbw * 64u, static_cast<uint32_t>(rowHi - rowLo + 1));
        g630MaterializeForRanges(r, G144BlockRange{}, !r.valid, false, kG261MatCpuReplay);
    }
    if (depth) g630MaterializeDepth();
}

// G632: the wanted-set construction, factored out so the immediate publisher and the epoch
// accumulator share ONE implementation instead of two paraphrases of it (G629's law).
static void g630BuildWanted(const G144BlockRange &src, const G144BlockRange &dst,
                            bool srcUnknown, bool dstUnknown,
                            std::array<uint8_t, kG630Pages> &wanted, bool countUnknown)
{
    if (srcUnknown || dstUnknown || (!src.valid && !dst.valid))
    {
        wanted.fill(1u);
        if (countUnknown) g_g630UnknownBreaks.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const auto add = [&](const G144BlockRange &r) {
        if (!r.valid) return;
        const uint32_t lo = std::min(kG630Pages - 1u, r.first >> 5u);
        const uint32_t hi = std::min(kG630Pages - 1u, r.last >> 5u);
        for (uint32_t p = lo; p <= hi; ++p) wanted[p] = 1u;
    };
    add(src); add(dst);
}

static bool g630WantedHitsAuthority(const std::array<uint8_t, kG630Pages> &wanted)
{
    for (uint32_t p = 0; p < kG630Pages; ++p)
        if (wanted[p] != 0u && g_g630GpuDirty[p] != 0u) return true;
    return false;
}

// G632: the same three operations on the 512-bit encoding. `g630BuildWantedBits` sets the same
// pages `g630BuildWanted` sets (the block->page shift and the clamp are copied from it verbatim),
// `g630BitsHitAuthority` is its intersection test in 8 words, and `g630ExpandBits` reproduces the
// byte array exactly for the rare path that actually publishes.
static void g630BuildWantedBits(const G144BlockRange &src, const G144BlockRange &dst,
                                bool srcUnknown, bool dstUnknown,
                                uint64_t bits[8], bool countUnknown)
{
    if (srcUnknown || dstUnknown || (!src.valid && !dst.valid))
    {
        for (int i = 0; i < 8; ++i) bits[i] = ~0ull;
        if (countUnknown) g_g630UnknownBreaks.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < 8; ++i) bits[i] = 0ull;
    const auto add = [&](const G144BlockRange &r) {
        if (!r.valid) return;
        const uint32_t lo = std::min(kG630Pages - 1u, r.first >> 5u);
        const uint32_t hi = std::min(kG630Pages - 1u, r.last >> 5u);
        for (uint32_t p = lo; p <= hi; ++p) bits[p >> 6] |= 1ull << (p & 63u);
    };
    add(src); add(dst);
}

static bool g630BitsHitAuthority(const uint64_t bits[8])
{
    for (int i = 0; i < 8; ++i)
        if ((bits[i] & g_g630DirtyBits[static_cast<size_t>(i)]) != 0ull) return true;
    return false;
}

static void g630ExpandBits(const uint64_t bits[8], std::array<uint8_t, kG630Pages> &wanted)
{
    for (uint32_t p = 0; p < kG630Pages; ++p)
        wanted[p] = ((bits[p >> 6] >> (p & 63u)) & 1ull) ? 1u : 0u;
}

// ===== G632: EPOCH SCOPES ======================================================================
// Open an epoch and every publication routed through g632EpochAdd is DEFERRED into one union set;
// closing it issues at most ONE round trip for the whole pass. See g632_coherence_epoch.inc for
// why each of the three call sites is (or is not) allowed to defer.
// An epoch is opened whenever the LEVER is armed for that pass OR the census is on. With the
// census alone it observes without deferring, so the "saved" column is measured on the SHIPPED
// path — the count of round trips a batched pass would have collapsed, taken from a run that did
// not collapse them (17e: a census must not move what it measures).
static void g632EpochBegin(int slot, bool defer)
{
    if (g_g632Ep.open) return; // never nest; a stray begin is a no-op, not a lost publication
    g_g632Ep.open = true;
    g_g632Ep.defer = defer;
    g_g632Ep.any = false;
    g_g632Ep.cause = 0;
    g_g632Ep.slot = slot;
    g_g632Ep.wanted.fill(0u);
    if (g632VerifyOn()) g_g632Ep.dirtyAtBegin = g_g630GpuDirty;
    if (g632StatOn()) ++g_g632EpPasses[static_cast<size_t>(slot) % 3u];
}

// The oracle for the batching premise: a page's authority bit is only ever SET by
// g630SubmitReplay and only ever CLEARED by publication, so inside a pass that submits no replay
// no page may go clean -> dirty. If that holds, the union published at the flush is the same page
// set the per-call publications would have covered, with the same content.
static void g632VerifyNoNewAuthority()
{
    if (!g632VerifyOn() || !g_g632Ep.open) return;
    for (uint32_t p = 0; p < kG630Pages; ++p)
    {
        ++g_g632VerifyChecks;
        if (g_g630GpuDirty[p] != 0u && g_g632Ep.dirtyAtBegin[p] == 0u) ++g_g632VerifyBad;
    }
}

// Returns true when the publication was CONSUMED (deferred to the flush); false means the caller
// must still publish now.
static bool g632EpochAdd(const std::array<uint8_t, kG630Pages> &wanted, bool hit, int cause)
{
    g632VerifyNoNewAuthority();
    if (hit && g632StatOn()) ++g_g632EpCalls[static_cast<size_t>(g_g632Ep.slot) % 3u];
    if (!hit) return g_g632Ep.defer;
    for (uint32_t p = 0; p < kG630Pages; ++p)
        if (wanted[p] != 0u) g_g632Ep.wanted[p] = 1u;
    g_g632Ep.any = true;
    // The break is attributed to the FIRST cause that demanded it, so a batched flush stays
    // comparable with the unbatched `[G630:break]` rows.
    if (g_g632Ep.cause == 0) g_g632Ep.cause = cause;
    return g_g632Ep.defer;
}

static void g632EpochEnd()
{
    if (!g_g632Ep.open) return;
    g632VerifyNoNewAuthority();
    const int slot = g_g632Ep.slot;
    const bool defer = g_g632Ep.defer;
    g_g632Ep.open = false;
    if (!g_g632Ep.any) return;
    if (g632StatOn())
    {
        ++g_g632EpFlushes[static_cast<size_t>(slot) % 3u];
        uint64_t pages = 0u;
        for (uint32_t p = 0; p < kG630Pages; ++p)
            if (g_g632Ep.wanted[p] != 0u && g_g630GpuDirty[p] != 0u) ++pages;
        g_g632EpPages[static_cast<size_t>(slot) % 3u] += pages;
    }
    g_g632Ep.any = false;
    if (!defer) return; // census-only epoch: every publication already went out individually
    g_g630CpuBreaks.fetch_add(1u, std::memory_order_relaxed);
    (void)g630ReadAndPublish(g_g632Ep.wanted, g_g632Ep.cause);
}

struct G632EpochScope
{
    bool armed;
    G632EpochScope(bool defer, int slot)
        : armed((defer || g632StatOn()) && !g_g632Ep.open)
    {
        if (armed) g632EpochBegin(slot, defer);
    }
    ~G632EpochScope() { if (armed) g632EpochEnd(); }
};

static void g630MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (!g630DomainOn() || !g630AnyDirty()) return;
    if (s_g632FastDirty && !g_g632Ep.open)
    {
        // G632 hot path: this runs once per TEXTURED DRAW (11.4 M times per `ridepod` run) and
        // answers "no" almost every time, so it must not cost a 512-byte zero-fill plus a
        // 512-byte scan to do it. Same pages, same decision, 8 words.
        uint64_t bits[8];
        g630BuildWantedBits(src, dst, srcUnknown, dstUnknown, bits, true);
        if (!g630BitsHitAuthority(bits)) return;
        std::array<uint8_t, kG630Pages> wanted{};
        g630ExpandBits(bits, wanted);
        g_g630CpuBreaks.fetch_add(1u, std::memory_order_relaxed);
        (void)g630ReadAndPublish(wanted, cause);
        return;
    }
    std::array<uint8_t, kG630Pages> wanted{};
    g630BuildWanted(src, dst, srcUnknown, dstUnknown, wanted, true);
    const bool hit = g630WantedHitsAuthority(wanted);
    // Inside an open epoch this publication is folded into the pass-wide union instead of being
    // issued now. Only sites proven not to touch a guest byte before the flush open one.
    if (g_g632Ep.open && g632EpochAdd(wanted, hit, cause))
        return;
    if (hit)
    {
        g_g630CpuBreaks.fetch_add(1u, std::memory_order_relaxed);
        (void)g630ReadAndPublish(wanted, cause);
    }
}

static void g630PrepareCpuReplay()
{
    if (!g630DomainOn() || g_g144List.empty()) return;
    // G632 epoch: this loop is PURE GS geometry — g144TargetRange / g254DepthSurface /
    // g144TextureRange / g144ClutRange read no guest VRAM byte and write none, and no replay is
    // submitted inside it, so no page can become GPU-authoritative between two iterations. The
    // union of the per-entry wanted sets is therefore the identical page set, published in one
    // round trip instead of up to 3N. g630MaterializeDepth() below is a different transfer
    // (the depth attachment) and stays outside the epoch.
    {
        G632EpochScope g632(g632EpochOn(), 0);
    for (const G144Entry &e : g_g144List)
    {
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        const G144BlockRange tex = e.prim.tme ? g144TextureRange(e) : G144BlockRange{};
        const G144BlockRange clut = (e.prim.tme && g144IsPalettedPsm(e.ctx.tex0.psm))
            ? g144ClutRange(e) : G144BlockRange{};
        const bool unknown = !color.valid || (depth.active && !depth.range.valid) ||
                             (e.prim.tme && !tex.valid);
        g630MaterializeForRanges(tex, color, unknown, unknown, 0);
        if (depth.active) g630MaterializeForRanges(G144BlockRange{}, depth.range, false,
                                                   !depth.range.valid, 0);
        if (clut.valid) g630MaterializeForRanges(clut, G144BlockRange{}, false, false, 0);
    }
    }
    g630MaterializeDepth();
}

static void g630PrepareGpuReplayInputs(const std::vector<G144Entry> &list)
{
    if (!g630DomainOn() || !g630AnyDirty()) return;
    // G632 epoch: same proof as g630PrepareCpuReplay — the body is pure range arithmetic and the
    // consumer (the GPU replay dispatch) runs after the whole pass.
    G632EpochScope g632(g632EpochOn(), 1);
    for (const G144Entry &e : list)
    {
        if (!e.prim.tme) continue;
        const G144BlockRange tex = g144TextureRange(e);
        g630MaterializeForRanges(tex, G144BlockRange{}, !tex.valid, false, kG630MatGpuInput);
        if (g144IsPalettedPsm(e.ctx.tex0.psm))
        {
            const G144BlockRange clut = g144ClutRange(e);
            g630MaterializeForRanges(clut, G144BlockRange{}, !clut.valid, false,
                                     kG630MatGpuInput);
        }
    }
}

// G632: every conjunct of the IMAGE-upload gate as one bitmask. `g630CanPatchUpload` is the
// shipped admission and is now literally `mask == 0`, so the census cannot drift from it.
static uint32_t g630UploadRejectMask(uint32_t block, uint32_t fbw, uint32_t psm,
                                     uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                     bool unknown)
{
    // Shipped-path guard: with the prototype off this is called on every upload edge, so it must
    // cost exactly what the original single `g630DomainOn()` test cost.
    if (!g630DomainOn()) return kG632UpOff;
    uint32_t m = 0u;
    if (!g_g630Initialized) m |= kG632UpUninit;
    if (!g630AnyDirty()) m |= kG632UpNoDirty;
    if (unknown) m |= kG632UpUnknown;
    if (psm != GS_PSM_CT32) m |= kG632UpPsm;
    if (fbw == 0u) m |= kG632UpFbw;
    if (width == 0u || height == 0u) m |= kG632UpEmpty;
    if ((m & (kG632UpUnknown | kG632UpFbw | kG632UpEmpty)) != 0u) return m | kG632UpRange;
    const G144BlockRange r = g144RangeForRect(block, psm, fbw, x, y, width, height);
    if (!r.valid) return m | kG632UpRange;
    bool dirty = false;
    for (uint32_t page = r.first >> 5u;
         page <= std::min(kG630Pages - 1u, r.last >> 5u); ++page)
        if (g_g630GpuDirty[page] != 0u) { dirty = true; break; }
    if (!dirty) m |= kG632UpClean;
    return m;
}

static bool g630CanPatchUpload(uint32_t block, uint32_t fbw, uint32_t psm,
                               uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               bool unknown, bool census = false)
{
    const uint32_t m = g630UploadRejectMask(block, fbw, psm, x, y, width, height, unknown);
    // Only an upload that would actually block (a dirty page under its rect) is an `up` break, so
    // the clean case is censused separately rather than polluting the reject histogram.
    if (census && (m & (kG632UpOff | kG632UpUninit | kG632UpNoDirty | kG632UpClean)) == 0u)
        g632NoteUploadReject(m);
    return m == 0u;
}

static void g630UploadCompleteInternal(uint8_t *vram, uint32_t block, uint32_t fbw,
                                       uint32_t psm, uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height)
{
    if (vram == nullptr) return;
    if (!g630CanPatchUpload(block, fbw, psm, x, y, width, height, false))
    {
        // G634b: the shipped scatter is CT32-only. Every other pixel format is expressible as
        // masked raw-VRAM word deltas, which needs no per-format shader — see
        // g634_raw_texture_view.inc. Declining here leaves the pre-G634 behaviour exactly.
        (void)g634IngestUpload(vram, block, fbw, psm, x, y, width, height);
        return;
    }
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    if (pixels == 0u || pixels > 1024u * 1024u) return;
    g630Poll(false);
    auto packet = std::make_shared<G630UploadPacket>();
    packet->block = block;
    packet->fbw = static_cast<int>(fbw);
    packet->x = static_cast<int>(x);
    packet->y = static_cast<int>(y);
    packet->width = static_cast<int>(width);
    packet->height = static_cast<int>(height);
    packet->pixels.resize(static_cast<size_t>(pixels));
    size_t n = 0u;
    for (uint32_t py = 0u; py < height; ++py)
        for (uint32_t px = 0u; px < width; ++px)
            packet->pixels[n++] = GSMem::ReadCT32(vram, block, fbw, x + px, y + py);

    G630Pending pending;
    pending.upload = packet;
    pending.view = true;
    if (!g630_backend_submit_upload(packet))
    {
        g630RecoverFailure(pending);
        return;
    }
    g_g630Pending.push_back(std::move(pending));
    const G144BlockRange r = g144RangeForRect(block, psm, fbw, x, y, width, height);
    if (g633PlansOn()) g633NoteRawRange(r, UINT32_MAX);
    // ⛔ G636: this used to stamp the whole PAGE as synced after scattering only the transferred
    // RECT into the raw image. A rect rarely covers a whole 64x32 page, so every other byte of that
    // page was then excluded from all future patches — permanently, because `syncedGen` had already
    // been advanced past the generation that would have triggered one. Stamp nothing: guest VRAM
    // holds the full correct page (the CPU wrote the rect before this hook ran), so the page is
    // simply re-patched the next time the domain needs it. `DC2_G636_NO_UPSYNCFIX=1` restores it.
    if (s_g636NoUpSyncFix && r.valid)
        for (uint32_t page = r.first >> 5u;
             page <= std::min(kG630Pages - 1u, r.last >> 5u); ++page)
            if (g_g630GpuDirty[page] == 0u)
                g_g630SyncedGen[page] = g_g178PageGen[page].load(std::memory_order_relaxed);
    g_g630Uploads.fetch_add(1u, std::memory_order_relaxed);
    g_g630UploadBytes.fetch_add(pixels * 4u, std::memory_order_relaxed);
}

struct G630UploadHookInstaller
{
    G630UploadHookInstaller() { g_g630UploadCompleteHook = &g630UploadCompleteInternal; }
};
static G630UploadHookInstaller g_g630UploadHookInstaller;

static void g630NoteCpuReplay()
{
    if (g630DomainOn()) { g_g630DepthValid = false; g_g630DepthDirty = false; }
}

static int g630SurfaceFbW(uint32_t fbp)
{
    if (g633PlansOn()) return g633SurfaceFbW(fbp);
    const auto it = g_g630Surfaces.find(fbp);
    return (it != g_g630Surfaces.end() && it->second.valid) ? it->second.fbW : 0;
}

static bool g630HaveSurfaces() { return !g_g630Surfaces.empty(); }

static uint64_t g630RangeGen(const G144BlockRange &r)
{
    if (!r.valid) return 0u;
    uint64_t sum = 0u;
    for (uint32_t p = r.first >> 5u; p <= std::min(kG630Pages - 1u, r.last >> 5u); ++p)
        sum += g_g178PageGen[p].load(std::memory_order_relaxed);
    return sum;
}

static bool g630PrepareView(G630Surface &s, const G144BlockRange &range,
                            int requestLo, int requestHi)
{
    // ⭐⭐⭐ G636: RETIRED. This decode is written into `ensureFbo(s.fbp, ...)` — the SAME FBO the
    // native renderer draws into, keyed by `fbp` alone — so it overwrites a whole row band of a
    // surface the raw image is not a superset of. Together with the resident replay's per-pixel
    // linear dual-write (`uWriteView`, retired in the backend by the same flag) this was the
    // measured `ridepod` chassis loss. `DC2_G636_LINVIEW=1` restores both verbatim.
    if (!s_g636LinView || s_g636NoCt32View) return false;
    requestLo = std::clamp(requestLo, 0, 511);
    requestHi = std::clamp(requestHi, requestLo, 511);
    const uint64_t globalGen = g_g149VramGen.load(std::memory_order_relaxed);
    const bool sameSeq = s.viewSeq == s.seq;
    const bool contained = sameSeq && requestLo >= s.viewLo && requestHi <= s.viewHi;
    if (contained && s.viewGlobalGen == globalGen)
        return true;
    const int desiredLo = sameSeq ? std::min(s.viewLo, requestLo) : requestLo;
    const int desiredHi = sameSeq ? std::max(s.viewHi, requestHi) : requestHi;
    // ⭐⭐⭐ G636 DEFECT A. This decode is written into `ensureFbo(s.fbp, ...)` — the SAME FBO the
    // native renderer draws into. If a dirty G261 target still owns any page of the band, its
    // pixels exist in neither the raw image nor guest VRAM and this write would destroy them.
    // Decline; the caller falls through to the shipped materialize/decode path.
    if (g636G261DirtyOverlaps(g636SurfaceBandRange(s.fbp, s.fbw, desiredLo, desiredHi)))
    {
        g_g636ArbCt32.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    auto packet = std::make_shared<G630ViewPacket>();
    packet->fbp = s.fbp; packet->fbw = static_cast<int>(s.fbw);
    packet->width = s.fbW; packet->height = 512;
    packet->originY = desiredLo;
    packet->rows = desiredHi - desiredLo + 1;
    static bool s_pageMapSubmitted = false;
    if (!s_pageMapSubmitted)
    {
        packet->pageWords.resize(64u * 32u);
        for (uint32_t y = 0u; y < 32u; ++y)
            for (uint32_t x = 0u; x < 64u; ++x)
            {
                const uint32_t word =
                    GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u;
                if (word >= 2048u) return false;
                packet->pageWords[y * 64u + x] = word;
            }
    }
    // ⭐⭐ G636: the DECODED BAND is what lands in the FBO, so the BAND is what must be reconciled
    // with guest VRAM first. The pre-G636 form patched only the sampled texture range and then wrote
    // rows outside it from raw content nothing had proven current.
    uint64_t gen;
    if (s_g636NoBandPatch)
    {
        g630AppendGuestPages(range.first >> 5u, (range.last >> 5u) - (range.first >> 5u) + 1u,
                             packet->patchPages, packet->patchWords);
        gen = g630RangeGen(range);
    }
    else
    {
        g636AppendSurfaceBandPages(s.fbp, s.fbw, desiredLo, desiredHi,
                                   packet->patchPages, packet->patchWords);
        gen = g636SurfaceBandGen(s.fbp, s.fbw, desiredLo, desiredHi);
    }
    if (contained && s.viewGen == gen && packet->patchPages.empty())
    {
        s.viewGlobalGen = globalGen;
        return true;
    }
    if (g630StatOn() && packet->rows >= 400)
    {
        static uint32_t s_largePrints = 0u;
        if (s_largePrints++ < 32u)
        {
            std::fprintf(stderr,
                         "[G630:viewwhy] fbp=%03x rows=%d..%d sameSeq=%u contained=%u "
                         "seq=%llu/%llu gen=%llu/%llu global=%llu/%llu patches=%zu\n",
                         s.fbp, packet->originY, packet->originY + packet->rows - 1,
                         sameSeq ? 1u : 0u, contained ? 1u : 0u,
                         (unsigned long long)s.viewSeq, (unsigned long long)s.seq,
                         (unsigned long long)s.viewGen, (unsigned long long)gen,
                         (unsigned long long)s.viewGlobalGen, (unsigned long long)globalGen,
                         packet->patchPages.size());
            if (g570BumpTraceOn() && !packet->patchPages.empty())
            {
                std::array<uint32_t, 32> sourcePages{};
                for (uint32_t page : packet->patchPages)
                {
                    const uint32_t source = g_g570LastBump[page & (kG178PageCount - 1u)].source;
                    ++sourcePages[std::min(source & 0xffu, 31u)];
                }
                std::fprintf(stderr, "[G630:viewsrc]");
                for (uint32_t source = 0u; source < sourcePages.size(); ++source)
                    if (sourcePages[source] != 0u)
                        std::fprintf(stderr, " %u=%u", source, sourcePages[source]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    if (!g630_backend_prepare_ct32_view(packet)) return false;
    s_pageMapSubmitted = true;
    g630CommitSyncedPages(packet->patchPages);
    G630Pending pending; pending.view = true;
    g_g630Pending.push_back(std::move(pending));
    s.viewSeq = s.seq; s.viewGen = gen; s.viewGlobalGen = globalGen;
    s.viewLo = desiredLo; s.viewHi = desiredHi;
    g_g630Views.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

// ===== G632: the CT32 direct-view admission, as a per-draw reject bitmask ======================
// Every conjunct of the shipped gate, in source order, with no short-circuit past the point where
// a later test would be undefined (nothing below `kG632BindNoSurface` is evaluable without a
// surface). `g630TryBind` is now `mask == 0` plus the one impure step, so the census reads the
// admission itself instead of a copy of it (G629's law).
static uint32_t g630BindRejectMask(uint32_t dstFbp, const G144Entry &e,
                                   const G178EntryState &st, bool alreadyBound,
                                   G630Surface **outSurface, int *outViewLo, int *outViewHi,
                                   G144BlockRange *outRange)
{
    if (outSurface) *outSurface = nullptr;
    uint32_t m = 0u;
    if (!g630DomainOn()) m |= kG632BindOff;
    if (alreadyBound) m |= kG632BindAlready;
    if (!e.prim.tme) m |= kG632BindNoTme;
    if (st.skip) m |= kG632BindSkip;
    if (e.ctx.tex0.psm != GS_PSM_CT32) m |= kG632BindPsm;
    if (!e.prim.fst) m |= kG632BindNoFst;
    // Every CHEAP conjunct above is evaluated unconditionally — that is the bitmask discipline.
    // The surface search and the window solve below are not cheap, and a draw that already failed
    // one of the tests above is fully explained by it, so they are skipped. The shipped path
    // (domain off) therefore still costs exactly one predicate.
    if (m != 0u) return m;
    G630Surface *sp = nullptr;
    for (auto &kv : g_g630Surfaces)
        if (kv.second.valid && kv.second.block == e.ctx.tex0.tbp0 &&
            kv.second.fbw == e.ctx.tex0.tbw) { sp = &kv.second; break; }
    if (sp == nullptr) return m | kG632BindNoSurface;
    if (outSurface) *outSurface = sp;
    if (sp->fbp == dstFbp) m |= kG632BindSelf;
    if (st.texW <= 0 || st.texH <= 0) m |= kG632BindTexDim;
    if (st.texW > sp->fbW) m |= kG632BindTexWide;
    if (sp->covHi < sp->covLo) m |= kG632BindNoCov;
    bool window = false;
    int viewLo = 0, viewHi = -1;
    if (st.bilinear && e.prim.type == GS_PRIM_SPRITE)
    {
        G623Surface tmp{};
        tmp.fbW = sp->fbW; tmp.fbH = 512; tmp.covLo = sp->covLo; tmp.covHi = sp->covHi;
        int taps[4]{};
        window = g623BilinearSpriteWindow(e, st, tmp, taps);
        if (window) { viewLo = taps[2]; viewHi = taps[3]; }
    }
    else if (!st.bilinear)
    {
        const int nv = e.prim.type == GS_PRIM_SPRITE ? 2 : 3;
        int loU = INT_MAX, hiU = INT_MIN, loV = INT_MAX, hiV = INT_MIN;
        for (int i = 0; i < nv; ++i)
        {
            loU = std::min(loU, static_cast<int>(e.v[i].u >> 4));
            hiU = std::max(hiU, static_cast<int>(e.v[i].u >> 4));
            loV = std::min(loV, static_cast<int>(e.v[i].v >> 4));
            hiV = std::max(hiV, static_cast<int>(e.v[i].v >> 4));
        }
        if (e.prim.type == GS_PRIM_SPRITE) { --hiU; --hiV; }
        window = loU >= 0 && hiU < sp->fbW && loV >= sp->covLo && hiV <= sp->covHi;
        if (window) { viewLo = loV; viewHi = hiV; }
    }
    if (!window) m |= kG632BindWindow;
    const G144BlockRange range = g144TextureRange(e);
    if (!range.valid || viewHi < viewLo) m |= kG632BindRange;
    if (outViewLo) *outViewLo = viewLo;
    if (outViewHi) *outViewHi = viewHi;
    if (outRange) *outRange = range;
    return m;
}

static uint32_t g630TryBind(uint32_t dstFbp, const G144Entry &e,
                            const G178EntryState &st, bool alreadyBound,
                            uint32_t *outMask = nullptr)
{
    // G636: with the resident linear FBO mirror retired there is no surface for this gate to bind,
    // and `[G632:gate]` prices its 15.0 M per-draw evaluations at 379 ms/run for **0 binds**. Cut
    // it at the top rather than letting the reject mask re-derive "no" fifteen million times.
    if (!s_g636LinView)
    {
        if (outMask) *outMask = kG632BindOff;
        return 0u;
    }
    G632GateTimer g632Timer(0);
    if (g633PlansOn() && st.residencyPlan != 0u)
        return g633TryBind(dstFbp, e, st, alreadyBound, outMask);
    G630Surface *sp = nullptr;
    int viewLo = 0, viewHi = -1;
    G144BlockRange range{};
    uint32_t m = g630BindRejectMask(dstFbp, e, st, alreadyBound, &sp, &viewLo, &viewHi, &range);
    if (m != 0u || sp == nullptr)
    {
        if (outMask) *outMask = m;
        return 0u;
    }
    if (!g630PrepareView(*sp, range, viewLo, viewHi))
    {
        if (outMask) *outMask = kG632BindView;
        return 0u;
    }
    if (outMask) *outMask = 0u;
    g_g630DirectBinds.fetch_add(1u, std::memory_order_relaxed);
    return sp->fbp;
}

static void g630BeginTexturePass()
{
    // G634's per-pass view set is independent of which coherence arm is running: a raw texture
    // view is only valid for the pass it was decoded in, in BOTH arms.
    g634BeginTexturePass();
    if (g633PlansOn()) { g633BeginTexturePass(); return; }
    g_g630T8Built.clear();
}

static bool g630RangeHasGpuAuthority(const G144BlockRange &r)
{
    if (!r.valid) return false;
    for (uint32_t p = r.first >> 5u;
         p <= std::min(kG630Pages - 1u, r.last >> 5u); ++p)
        if (g_g630GpuDirty[p] != 0u) return true;
    return false;
}

static void g630AppendGuestRangeUnique(const G144BlockRange &r, G630T8Packet &packet)
{
    if (!r.valid) return;
    std::vector<uint32_t> pages, words;
    g630AppendGuestPages(r.first >> 5u, (r.last >> 5u) - (r.first >> 5u) + 1u,
                         pages, words);
    for (size_t i = 0; i < pages.size(); ++i)
    {
        if (std::find(packet.patchPages.begin(), packet.patchPages.end(), pages[i]) !=
            packet.patchPages.end())
            continue;
        packet.patchPages.push_back(pages[i]);
        packet.patchWords.insert(packet.patchWords.end(),
                                 words.begin() + i * kG630WordsPerPage,
                                 words.begin() + (i + 1u) * kG630WordsPerPage);
    }
}

// ===== G632: the raw PSMT8 direct-view admission, as a per-draw reject bitmask =================
// Same discipline as g630BindRejectMask: all cheap conjuncts unconditionally, then the range work
// only for draws that survived them.
static uint32_t g630T8RejectMask(const G144Entry &e, const G178EntryState &st,
                                 G144BlockRange *outTex, G144BlockRange *outClut)
{
    if (!g630DomainOn()) return kG632T8Off;
    uint32_t m = 0u;
    if (!g_g630Initialized) m |= kG632T8Uninit;
    if (!e.prim.tme) m |= kG632T8NoTme;
    if (st.skip) m |= kG632T8Skip;
    if (e.ctx.tex0.psm != GS_PSM_T8) m |= kG632T8Psm;
    if (st.texKey == 0u) m |= kG632T8NoKey;
    if (st.texW <= 0 || st.texH <= 0 || st.texW > 1024 || st.texH > 1024) m |= kG632T8Dim;
    if (m != 0u) return m;
    const G144BlockRange tex = g144TextureRange(e);
    if (outTex) *outTex = tex;
    if (!tex.valid) return m | kG632T8Range;
    if (!g630RangeHasGpuAuthority(tex)) m |= kG632T8NoAuthority;
    const G144BlockRange clut = g144ClutRange(e);
    if (outClut) *outClut = clut;
    if (!clut.valid) m |= kG632T8Clut;
    return m;
}

#include "g633_compiled_residency.inc"

static uint64_t g630TryT8View(const G144Entry &e, const G178EntryState &st,
                              uint32_t *outMask = nullptr)
{
    G632GateTimer g632Timer(1);
    if (g633PlansOn() && st.residencyPlan != 0u)
        return g633TryT8View(e, st, outMask);
    G144BlockRange tex{}, clut{};
    const uint32_t m = g630T8RejectMask(e, st, &tex, &clut);
    if (m != 0u)
    {
        if (outMask) *outMask = m;
        return 0u;
    }
    // G636: the indexed image or its CLUT may still be live in a dirty G261 FBO, which this decode
    // cannot see. Decline so the shipped path materialises that target first.
    if (s_g636NoT8View ||
        g636G261DirtyOverlaps(tex) || g636G261DirtyOverlaps(clut))
    {
        g_g636ArbT8.fetch_add(1u, std::memory_order_relaxed);
        if (outMask) *outMask = kG632T8Build;
        return 0u;
    }
    if (outMask) *outMask = kG632T8Build; // cleared below once the view is actually built

    // Keep the established exact CLUT helper. Only a GPU-authored CLUT is published; the much
    // larger indexed image remains private in the raw GPU domain.
    g630MaterializeForRanges(clut, G144BlockRange{}, false, false, kG261MatTexAlias);
    if (!g630DomainOn()) return 0u;

    const uint64_t key = g281ViewKey(st.texKey);
    if (g_g630T8Built.count(key) != 0u) { if (outMask) *outMask = 0u; return key; }
    auto packet = std::make_shared<G630T8Packet>();
    packet->texKey = key;
    packet->tbp = e.ctx.tex0.tbp0;
    packet->tbw = std::max(1u, static_cast<uint32_t>(e.ctx.tex0.tbw) >> 1u);
    packet->width = st.texW;
    packet->height = st.texH;
    static bool s_permSubmitted = false;
    const G618P8Page &p8 = g618P8Page();
    if (!p8.ok) return 0u;
    if (!s_permSubmitted)
        packet->mapWords.assign(p8.perm.begin(), p8.perm.end());
    packet->clut.resize(256u);
    for (uint32_t ci = 0u; ci < 256u; ++ci)
        packet->clut[ci] = g178LookupClutFixed(
            g_g630Vram, e.texa, e.texclut, static_cast<uint8_t>(ci), e.ctx.tex0.cbp,
            e.ctx.tex0.cpsm, e.ctx.tex0.csm, e.ctx.tex0.csa, e.ctx.tex0.psm);
    g630AppendGuestRangeUnique(tex, *packet);
    g630AppendGuestRangeUnique(clut, *packet);

    G630Pending pending;
    pending.t8 = packet;
    pending.view = true;
    if (!g630_backend_prepare_t8_view(packet))
    {
        g630RecoverFailure(pending);
        return 0u;
    }
    s_permSubmitted = true;
    g630CommitSyncedPages(packet->patchPages);
    g_g630Pending.push_back(std::move(pending));
    g_g630T8Built.insert(key);
    g_g630T8Views.fetch_add(1u, std::memory_order_relaxed);
    g_g630T8Pixels.fetch_add(static_cast<uint64_t>(st.texW) * st.texH,
                             std::memory_order_relaxed);
    if (outMask) *outMask = 0u;
    return key;
}

// G634 reuses g630AppendGuestPages / g630CommitSyncedPages / g630RangeHasGpuAuthority /
// g630RecoverFailure and the pending queue, so it follows all of them.
#include "g634_raw_texture_view.inc"
