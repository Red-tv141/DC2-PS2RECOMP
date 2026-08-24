// G652 P12: diagnostic-only packed-GIF descriptor-sequence census.
// The hot decoder calls this only inside the pre-existing DC2_G650_GIF_STAT branch.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace
{
    struct SequenceCount
    {
        uint64_t descriptorsKey = 0u;
        uint32_t nreg = 0u;
        uint64_t tags = 0u;
        uint64_t descriptors = 0u;
    };

    struct SequenceKey
    {
        uint64_t descriptors = 0u;
        uint32_t nreg = 0u;
        bool operator==(const SequenceKey &other) const
        {
            return descriptors == other.descriptors && nreg == other.nreg;
        }
    };

    struct SequenceHash
    {
        size_t operator()(const SequenceKey &key) const
        {
            return static_cast<size_t>(key.descriptors ^ (key.descriptors >> 32u) ^
                                       (static_cast<uint64_t>(key.nreg) * 0x9E3779B97F4A7C15ull));
        }
    };

    std::unordered_map<SequenceKey, SequenceCount, SequenceHash> s_sequences;
    uint64_t s_totalTags = 0u;
    uint64_t s_totalDescriptors = 0u;
    uint64_t s_nextReport = 4000000u;
}

void g652GifSequenceNote(uint64_t tagHi, uint32_t nreg, uint32_t descriptors)
{
    const uint32_t bits = nreg >= 16u ? 64u : nreg * 4u;
    const uint64_t mask = bits == 64u ? ~0ull : ((1ull << bits) - 1ull);
    const SequenceKey key{tagHi & mask, nreg};
    SequenceCount &entry = s_sequences[key];
    entry.descriptorsKey = key.descriptors;
    entry.nreg = nreg;
    ++entry.tags;
    entry.descriptors += descriptors;
    ++s_totalTags;
    s_totalDescriptors += descriptors;
    if (s_totalDescriptors < s_nextReport)
        return;
    s_nextReport = s_totalDescriptors + 4000000u;

    std::vector<SequenceCount> ranked;
    ranked.reserve(s_sequences.size());
    for (const auto &kv : s_sequences)
        ranked.push_back(kv.second);
    std::sort(ranked.begin(), ranked.end(), [](const SequenceCount &a, const SequenceCount &b) {
        return a.descriptors > b.descriptors;
    });
    std::fprintf(stderr, "[G652:gifseq] tags=%llu desc=%llu unique=%zu top:",
                 static_cast<unsigned long long>(s_totalTags),
                 static_cast<unsigned long long>(s_totalDescriptors), ranked.size());
    const size_t count = std::min<size_t>(8u, ranked.size());
    for (size_t i = 0u; i < count; ++i)
    {
        const SequenceCount &e = ranked[i];
        std::fprintf(stderr, " nreg%u/0x%llx=%.2f%%(%llu tags)", e.nreg,
                     static_cast<unsigned long long>(e.descriptorsKey),
                     100.0 * static_cast<double>(e.descriptors) /
                         static_cast<double>(s_totalDescriptors),
                     static_cast<unsigned long long>(e.tags));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}
