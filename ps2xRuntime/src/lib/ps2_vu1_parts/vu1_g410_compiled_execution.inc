// G410 - cached compiled execution for VU1 upper micro-ops.
//
// The VU1 code store is writable through VIF MPG, so a cache keyed only by PC is unsafe.
// VIF1 is the runtime's sole VU-code write funnel. It precisely invalidates touched slots on
// both the synchronous code store and the MTVU worker's ordered replay. Verification adds an
// exact 64-bit source guard on every hit to prove that the write boundary remains complete.
//
// Default:  enabled
// Kill:     DC2_G410_NO_VU_COMPILED=1
// Verify:   DC2_G410_VERIFY=1 (shadow-executes the legacy upper decoder)
// Stats:    DC2_G410_STAT=1

#include <array>

namespace
{
    struct G410CompiledUpper
    {
        uint8_t upperVfDest = 0xffu;
        uint8_t upperKind = 0xffu;
        uint8_t lowerFs = 0u;
        uint8_t lowerFt = 0u;
        uint8_t pairFlags = 0u;
        uint8_t lowerS2 = 0xffu;
        uint8_t reserved0 = 0u;
        uint8_t reserved1 = 0u;
    };
    static_assert(sizeof(G410CompiledUpper) == 8u);

    constexpr uint8_t G410_PAIR_E_BIT = 0x01u;
    constexpr uint8_t G410_PAIR_I_BIT = 0x02u;

    struct G410CompiledCache
    {
        const uint8_t *codeBase = nullptr;
        std::array<G410CompiledUpper, PS2_VU1_CODE_SIZE / 8u> uppers{};
        std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> sources{};
        uint64_t hits = 0u;
        uint64_t compiles = 0u;
        uint64_t upperFallbacks = 0u;
        uint64_t verified = 0u;
        uint64_t verifyBad = 0u;
        uint64_t invalidations = 0u;
        uint64_t invalidatedPairs = 0u;
        uint64_t lastReportOps = 0u;
    };

    thread_local G410CompiledCache s_g410Cache;

    bool g410CompiledRequested()
    {
        static const bool s_on =
            !f50_12_vu_env_flag("DC2_G410_NO_VU_COMPILED");
        return s_on;
    }

    bool g410VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G410_VERIFY");
        return s_on;
    }

    bool g410StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G410_STAT");
        return s_on;
    }

    bool g410UpperDiagnosticsCompatible()
    {
        // These probes live inside execUpper itself. The run-loop master diagnostic gate
        // handles the other per-pair probes and disables G410 at the call site.
        static const bool s_ok =
            std::getenv("DC2_G65_CLIP") == nullptr;
        return s_ok;
    }

    uint8_t g410SelectUpperKind(uint32_t upper)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3fu);
        if (op <= 0x2fu)
            return op;

        if (op < 0x3cu)
            return 0xffu;

        const uint8_t func = S2_FUNC(upper);
        return func <= 0x31u && func != 0x2bu
            ? static_cast<uint8_t>(0x40u + func)
            : 0xffu;
    }

    G410CompiledUpper g410DecodePair(uint64_t source)
    {
        const uint32_t upper = static_cast<uint32_t>(source >> 32u);
        const uint32_t lower = static_cast<uint32_t>(source);
        G410CompiledUpper pair{};
        pair.upperKind = g410SelectUpperKind(upper);
        pair.lowerFs = FS(lower);
        pair.lowerFt = FT(lower);
        if ((upper & (1u << 30u)) != 0u)
            pair.pairFlags |= G410_PAIR_E_BIT;
        if ((upper & (1u << 31u)) != 0u)
            pair.pairFlags |= G410_PAIR_I_BIT;
        if (((lower >> 25u) & 0x7fu) == 0x40u)
            pair.lowerS2 = S2_FUNC(lower);

        const uint8_t upperOp = static_cast<uint8_t>(upper & 0x3fu);
        if (upperOp < 0x3cu)
            pair.upperVfDest = FD(upper);
        else
        {
            const uint8_t upperS2 = S2_FUNC(upper);
            if ((upperS2 >= 0x10u && upperS2 <= 0x17u) ||
                upperS2 == 0x1du)
                pair.upperVfDest = FT(upper);
        }
        return pair;
    }

    void g410CompilePairSlow(
        size_t index, const uint8_t *codeBase, bool countStats)
    {
        uint64_t source = 0u;
        std::memcpy(&source, codeBase + index * sizeof(source), sizeof(source));
        const G410CompiledUpper pair = g410DecodePair(source);
        s_g410Cache.sources[index] = source;
        s_g410Cache.uppers[index] = pair;
        if (countStats)
        {
            ++s_g410Cache.compiles;
            if (pair.upperKind == 0xffu)
                ++s_g410Cache.upperFallbacks;
        }
    }

    G410CompiledUpper *g410PreparePairs(const uint8_t *codeBase, bool countStats)
    {
        if (s_g410Cache.codeBase != codeBase)
        {
            s_g410Cache.codeBase = codeBase;
            for (size_t index = 0u; index < s_g410Cache.uppers.size(); ++index)
                g410CompilePairSlow(index, codeBase, countStats);
        }
        return s_g410Cache.uppers.data();
    }

    [[noreturn]] void g410MissedInvalidation(
        uint32_t pc, uint64_t cachedSource, uint64_t liveSource)
    {
        ++s_g410Cache.verifyBad;
        std::fprintf(stderr,
                     "[G410:verify] missed code invalidation pc=0x%x "
                     "cached=%016llx live=%016llx\n",
                     pc,
                     static_cast<unsigned long long>(cachedSource),
                     static_cast<unsigned long long>(liveSource));
        std::abort();
    }

    [[noreturn]] void g410MismatchedPredecode(
        uint32_t pc, const G410CompiledUpper &cached, const G410CompiledUpper &expected)
    {
        ++s_g410Cache.verifyBad;
        uint64_t cachedBits = 0u;
        uint64_t expectedBits = 0u;
        std::memcpy(&cachedBits, &cached, sizeof(cachedBits));
        std::memcpy(&expectedBits, &expected, sizeof(expectedBits));
        std::fprintf(stderr,
                     "[G410:verify] mismatched predecode pc=0x%x "
                     "cached=%016llx expected=%016llx\n",
                     pc,
                     static_cast<unsigned long long>(cachedBits),
                     static_cast<unsigned long long>(expectedBits));
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g410VerifyCachedPair(
        uint32_t pc, uint64_t liveSource, const G410CompiledUpper &pair)
    {
        const size_t index = pc >> 3u;
        if (s_g410Cache.sources[index] != liveSource)
            g410MissedInvalidation(pc, s_g410Cache.sources[index], liveSource);
        const G410CompiledUpper expected = g410DecodePair(liveSource);
        if (std::memcmp(&pair, &expected, sizeof(pair)) != 0)
            g410MismatchedPredecode(pc, pair, expected);
    }

    DC2_G330_FORCEINLINE G410CompiledUpper &g410FetchPair(
        G410CompiledUpper *uppers, const uint8_t *codeBase, uint32_t pc,
        bool countStats, bool verifyCache, uint32_t &lower, uint32_t &upper)
    {
        const size_t index = pc >> 3u;
        G410CompiledUpper &pair = uppers[index];
        uint64_t liveSource = 0u;
        std::memcpy(&liveSource, codeBase + pc, sizeof(liveSource));
        if (countStats)
            ++s_g410Cache.hits;
        if (verifyCache)
            g410VerifyCachedPair(pc, liveSource, pair);
        lower = static_cast<uint32_t>(liveSource);
        upper = static_cast<uint32_t>(liveSource >> 32u);
        return pair;
    }

    bool g410UpperStatesEqual(const VU1State &a, const VU1State &b)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 &&
               a.mac == b.mac && a.status == b.status && a.clip == b.clip;
    }

    void g410ReportMaybe()
    {
        if (!g410StatOn())
            return;
        const uint64_t ops = s_g410Cache.hits + s_g410Cache.compiles;
        if (ops - s_g410Cache.lastReportOps < 5000000u)
            return;
        s_g410Cache.lastReportOps = ops;
        const double hitPct = ops != 0u
            ? 100.0 * static_cast<double>(s_g410Cache.hits) / static_cast<double>(ops)
            : 0.0;
        std::fprintf(stderr,
                     "[G410:jit] ops=%llu hit=%llu compile=%llu hitPct=%.3f "
                     "upperFallback=%llu invalidations=%llu invalidatedPairs=%llu "
                     "verified=%llu bad=%llu\n",
                     static_cast<unsigned long long>(ops),
                     static_cast<unsigned long long>(s_g410Cache.hits),
                     static_cast<unsigned long long>(s_g410Cache.compiles),
                     hitPct,
                     static_cast<unsigned long long>(s_g410Cache.upperFallbacks),
                     static_cast<unsigned long long>(s_g410Cache.invalidations),
                     static_cast<unsigned long long>(s_g410Cache.invalidatedPairs),
                     static_cast<unsigned long long>(s_g410Cache.verified),
                     static_cast<unsigned long long>(s_g410Cache.verifyBad));
    }
} // namespace

// Called from the only two code-write sites: VIF1's live MPG store and MTVU's ordered worker
// replay. Both calls execute on the same thread that can consume the corresponding code buffer,
// so the thread-local compiled cache needs no cross-thread synchronization.
void dc2_g410_vu_code_written(const uint8_t *codeBase, uint32_t offset, uint32_t sizeBytes)
{
    if (codeBase == nullptr || sizeBytes == 0u || s_g410Cache.codeBase != codeBase)
        return;

    const uint64_t end64 = static_cast<uint64_t>(offset) + sizeBytes;
    if (offset >= PS2_VU1_CODE_SIZE)
        return;
    const uint32_t end = static_cast<uint32_t>(
        end64 > PS2_VU1_CODE_SIZE ? PS2_VU1_CODE_SIZE : end64);
    const size_t first = static_cast<size_t>(offset >> 3u);
    const size_t last = static_cast<size_t>((end - 1u) >> 3u);
    uint64_t invalidated = 0u;
    for (size_t index = first; index <= last; ++index)
    {
        uint64_t liveSource = 0u;
        std::memcpy(
            &liveSource, codeBase + index * sizeof(liveSource), sizeof(liveSource));
        if (s_g410Cache.sources[index] != liveSource)
        {
            g410CompilePairSlow(index, codeBase, g410StatOn());
            ++invalidated;
        }
    }
    if (g410StatOn())
    {
        ++s_g410Cache.invalidations;
        s_g410Cache.invalidatedPairs += invalidated;
    }
}
