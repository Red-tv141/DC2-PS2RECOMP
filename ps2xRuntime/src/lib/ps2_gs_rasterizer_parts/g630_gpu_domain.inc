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

static bool g630AnyDirty()
{
    for (uint8_t v : g_g630GpuDirty)
        if (v != 0u) return true;
    return false;
}

static void g630Report()
{
    if (!g630StatOn()) return;
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
                g_g630GpuDirty[page] = 0u;
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
    g_g630GpuDirty.fill(0u);
    g_g630Surfaces.clear();
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
    if (!g_g630Initialized)
    {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(g_g630Vram);
        packet->seedVram.assign(src, src + (4u * 1024u * 1024u / 4u));
    }
    else
        g630AppendGuestPages(firstPage, pageCount, packet->patchPages, packet->patchWords);
    if (packet->wantDepth && !g_g630DepthValid)
        packet->depthSeed.assign(g403DisplayZBuf().begin(), g403DisplayZBuf().end());
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
    for (uint32_t p = firstPage; p < last; ++p) g_g630GpuDirty[p] = 1u;
    if (packet->wantDepth) { g_g630DepthValid = true; g_g630DepthDirty = true; }
    G630Surface &s = g_g630Surfaces[fbp];
    s.fbp = fbp; s.block = fbp << 5u; s.fbw = fbw; s.fbW = static_cast<int>(fbw) * 64;
    s.covLo = 0; s.covHi = std::max(s.covHi, height - 1); s.seq = ++g_g630Seq; s.valid = true;
    // The exact replay dispatch writes the raw word and its linear FBO mirror together. The
    // mirror is therefore current for every dispatched row at this sequence point in the FIFO.
    s.viewSeq = s.seq;
    s.viewLo = 0;
    // Rows above this replay's maxY were not touched in raw VRAM either; their prior linear
    // mirror remains exact. Preserve the accumulated coverage instead of forcing a full rebuild.
    s.viewHi = std::max(s.viewHi, std::min(511, height - 1));
    s.viewGlobalGen = g_g149VramGen.load(std::memory_order_relaxed);
    s.viewGen = 0u;
    for (uint32_t p = firstPage; p < last; ++p)
        s.viewGen += g_g178PageGen[p].load(std::memory_order_relaxed);
    G630Pending pending;
    pending.packet = packet;
    pending.gs = gs;
    pending.fallback.swap(list);
    g_g630Pending.push_back(std::move(pending));
    const uint64_t n = g_g630Submitted.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 256u) == 0u) g630Report();
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

static bool g630ReadAndPublish(const std::array<uint8_t, kG630Pages> &wanted, int cause)
{
    const auto t0 = std::chrono::steady_clock::now();
    g630Poll(true);
    if (g_g630Fault || g_g630Vram == nullptr) return false;
    uint64_t pages = 0u;
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
            if (!g630_backend_read_vram_rows(p, count, band)) return false;
            // The backend falls back to the whole image when it cannot take the band path; the
            // only signal is the size, so re-base the source offset accordingly.
            const bool full = band.size() >= static_cast<size_t>(kG630Pages) * kG630WordsPerPage;
            if (!full && band.size() != static_cast<size_t>(count) * kG630WordsPerPage)
                return false;
            const uint32_t base = full ? 0u : p;
            g_g631ReadCalls.fetch_add(1u, std::memory_order_relaxed);
            g_g631ReadRows.fetch_add(full ? kG630Pages : count, std::memory_order_relaxed);
            if (full) g_g631ReadFull.fetch_add(1u, std::memory_order_relaxed);
            for (uint32_t r = p; r < q; ++r)
            {
                if (wanted[r] == 0u || g_g630GpuDirty[r] == 0u) continue; // bridged gap page
                std::memcpy(g_g630Vram + static_cast<size_t>(r) * 8192u,
                            band.data() + static_cast<size_t>(r - base) * kG630WordsPerPage,
                            8192u);
                g_g630GpuDirty[r] = 0u;
                const uint64_t gen = g_g178PageGen[r].fetch_add(1u, std::memory_order_relaxed) + 1u;
                g_g630SyncedGen[r] = gen;
                ++pages;
            }
            any = true;
            p = q;
        }
        (void)any;
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
        g_g630GpuDirty[p] = 0u;
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
    }
    else
        g_g630Fault = true;
}

static void g630MaterializeForRanges(const G144BlockRange &src, const G144BlockRange &dst,
                                     bool srcUnknown, bool dstUnknown, int cause)
{
    if (!g630DomainOn() || !g630AnyDirty()) return;
    std::array<uint8_t, kG630Pages> wanted{};
    if (srcUnknown || dstUnknown || (!src.valid && !dst.valid))
    {
        wanted.fill(1u);
        g_g630UnknownBreaks.fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        const auto add = [&](const G144BlockRange &r) {
            if (!r.valid) return;
            const uint32_t lo = std::min(kG630Pages - 1u, r.first >> 5u);
            const uint32_t hi = std::min(kG630Pages - 1u, r.last >> 5u);
            for (uint32_t p = lo; p <= hi; ++p) wanted[p] = 1u;
        };
        add(src); add(dst);
    }
    bool hit = false;
    for (uint32_t p = 0; p < kG630Pages; ++p) hit = hit || (wanted[p] && g_g630GpuDirty[p]);
    if (hit)
    {
        g_g630CpuBreaks.fetch_add(1u, std::memory_order_relaxed);
        (void)g630ReadAndPublish(wanted, cause);
    }
}

static void g630PrepareCpuReplay()
{
    if (!g630DomainOn() || g_g144List.empty()) return;
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
    g630MaterializeDepth();
}

static void g630PrepareGpuReplayInputs(const std::vector<G144Entry> &list)
{
    if (!g630DomainOn() || !g630AnyDirty()) return;
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

static bool g630CanPatchUpload(uint32_t block, uint32_t fbw, uint32_t psm,
                               uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               bool unknown)
{
    if (!g630DomainOn() || !g_g630Initialized || !g630AnyDirty() || unknown ||
        psm != GS_PSM_CT32 || fbw == 0u || width == 0u || height == 0u)
        return false;
    const G144BlockRange r = g144RangeForRect(block, psm, fbw, x, y, width, height);
    if (!r.valid) return false;
    for (uint32_t page = r.first >> 5u;
         page <= std::min(kG630Pages - 1u, r.last >> 5u); ++page)
        if (g_g630GpuDirty[page] != 0u) return true;
    return false;
}

static void g630UploadCompleteInternal(uint8_t *vram, uint32_t block, uint32_t fbw,
                                       uint32_t psm, uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height)
{
    if (vram == nullptr || !g630CanPatchUpload(block, fbw, psm, x, y, width, height, false))
        return;
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
    if (r.valid)
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
    requestLo = std::clamp(requestLo, 0, 511);
    requestHi = std::clamp(requestHi, requestLo, 511);
    const uint64_t globalGen = g_g149VramGen.load(std::memory_order_relaxed);
    const bool sameSeq = s.viewSeq == s.seq;
    const bool contained = sameSeq && requestLo >= s.viewLo && requestHi <= s.viewHi;
    if (contained && s.viewGlobalGen == globalGen)
        return true;
    const int desiredLo = sameSeq ? std::min(s.viewLo, requestLo) : requestLo;
    const int desiredHi = sameSeq ? std::max(s.viewHi, requestHi) : requestHi;
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
    g630AppendGuestPages(range.first >> 5u, (range.last >> 5u) - (range.first >> 5u) + 1u,
                         packet->patchPages, packet->patchWords);
    const uint64_t gen = g630RangeGen(range);
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

static uint32_t g630TryBind(uint32_t dstFbp, const G144Entry &e,
                            const G178EntryState &st, bool alreadyBound)
{
    if (!g630DomainOn() || alreadyBound || !e.prim.tme || st.skip ||
        e.ctx.tex0.psm != GS_PSM_CT32 || !e.prim.fst)
        return 0u;
    G630Surface *sp = nullptr;
    for (auto &kv : g_g630Surfaces)
        if (kv.second.valid && kv.second.block == e.ctx.tex0.tbp0 &&
            kv.second.fbw == e.ctx.tex0.tbw) { sp = &kv.second; break; }
    if (sp == nullptr || sp->fbp == dstFbp || st.texW <= 0 || st.texH <= 0 ||
        st.texW > sp->fbW || sp->covHi < sp->covLo)
        return 0u;
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
    if (!window) return 0u;
    const G144BlockRange range = g144TextureRange(e);
    if (!range.valid || viewHi < viewLo || !g630PrepareView(*sp, range, viewLo, viewHi))
        return 0u;
    g_g630DirectBinds.fetch_add(1u, std::memory_order_relaxed);
    return sp->fbp;
}

static void g630BeginTexturePass()
{
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

static uint64_t g630TryT8View(const G144Entry &e, const G178EntryState &st)
{
    if (!g630DomainOn() || !g_g630Initialized || !e.prim.tme || st.skip ||
        e.ctx.tex0.psm != GS_PSM_T8 || st.texKey == 0u || st.texW <= 0 || st.texH <= 0 ||
        st.texW > 1024 || st.texH > 1024)
        return 0u;
    const G144BlockRange tex = g144TextureRange(e);
    if (!tex.valid || !g630RangeHasGpuAuthority(tex)) return 0u;
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid) return 0u;

    // Keep the established exact CLUT helper. Only a GPU-authored CLUT is published; the much
    // larger indexed image remains private in the raw GPU domain.
    g630MaterializeForRanges(clut, G144BlockRange{}, false, false, kG261MatTexAlias);
    if (!g630DomainOn()) return 0u;

    const uint64_t key = g281ViewKey(st.texKey);
    if (g_g630T8Built.count(key) != 0u) return key;
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
    return key;
}
