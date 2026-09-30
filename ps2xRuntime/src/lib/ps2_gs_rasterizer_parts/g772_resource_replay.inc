// A single unsupported primitive used to demote an entire captured batch to CPU
// replay. That demotion materializes its GPU depth attachment before even the
// GPU-compatible depth consumers can run. Preserve submission order, but keep
// compatible runs on the GPU. CPU materialization is scoped to the actual run
// that needs it. This is a scheduling change, not wider shader admission.
namespace
{
thread_local bool t_g772ResourceReplay = false;
static bool g772ResourceReplayOn()
{
    static const bool enabled = envFlagEnabled("DC2_G772_RESOURCE_REPLAY") &&
                                !envFlagEnabled("DC2_G772_NO_RESOURCE_REPLAY");
    return enabled;
}

static bool g772ReplayResourceRuns(bool sequential)
{
    if (!g772ResourceReplayOn() || t_g772ResourceReplay || g_g144List.size() < 2 ||
        !g203UniversalZEnabled() || !g178GpuOn() ||
        g_g266FlushRej != kG266FrClassify) return false;
    const auto& replay = sequential ? g_g144FlushSeqFn : g_g144FlushFn;
    if (!replay) return false;
    // Classify without executing or relaxing an admission predicate. The actual
    // run still uses g178TryFlushGpu and falls back through the canonical replay.
    std::vector<uint8_t> eligible;
    eligible.reserve(g_g144List.size());
    bool gpu = false, cpu = false;
    for (const auto& entry : g_g144List)
    {
        G178EntryState state{};
        const bool accepted = g178ClassifyEntry(entry, state);
        eligible.push_back(accepted);
        gpu |= accepted; cpu |= !accepted;
    }
    if (!gpu || !cpu) return false;

    auto entries = std::move(g_g144List);
    const auto* oldRead = g_g528Rd;
    const auto* oldWrite = g_g528Wr;
    auto oldOpenRead = g_g260OpenRd;
    auto oldOpenWrite = g_g260OpenWr;
    struct Restore
    {
        const G260RangeSet* read;
        const G260RangeSet* write;
        G260RangeSet openRead, openWrite;
        ~Restore()
        {
            g_g528Rd = read; g_g528Wr = write;
            g_g260OpenRd = openRead; g_g260OpenWr = openWrite;
            t_g772ResourceReplay = false;
        }
    } restore{oldRead, oldWrite, oldOpenRead, oldOpenWrite};
    t_g772ResourceReplay = true;
    for (size_t first = 0; first < entries.size();)
    {
        size_t end = first + 1;
        while (end < entries.size() && eligible[end] == eligible[first]) ++end;
        g_g144List.assign(entries.begin() + first, entries.begin() + end);
        // Build range authority for this resource consumer only. Reusing the
        // parent batch union here would reintroduce unrelated publication waits.
        g_g260OpenRd = {}; g_g260OpenWr = {};
        for (const auto& entry : g_g144List) g260NoteAppend(entry);
        G260RangeSet read = g_g260OpenRd, write = g_g260OpenWr;
        g_g528Rd = &read; g_g528Wr = &write;
        replay();
        first = end;
    }
    g_g144List.clear();
    return true;
}
}
