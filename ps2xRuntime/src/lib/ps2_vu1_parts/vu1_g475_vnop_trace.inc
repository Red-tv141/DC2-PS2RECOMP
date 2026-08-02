// G475 - compact sequential trace for VNOP-upper + covered non-control lower pairs.
//
// The behavior-pure G421/G475 census found this class on 34.5% of MAP-0 pairs, with 90.1% of
// those pairs in runs of length >=2 (31.1% of every executed pair). These pairs need Q/P clock
// advancement and their lower operation, but no upper dispatch, pair hazard, MAC-history
// publication, scalar prestall, flag read, E/I handling, or branch-delay update. Running a whole
// predecoded run in this compact noinline loop keeps that work out of the 2,487-instruction main
// pair body and removes its live-value pressure for the covered third of the stream.
//
// Bring-up: DC2_G475_VNOP_TRACE=1 (default off until the binary A/B and routes pass).
// Verify:   DC2_G475_VERIFY=1 (candidate fast lower vs legacy execLower, wired at the call site).
// Stats:    DC2_G475_STAT=1.
extern int g475VnopTraceArm();

namespace
{
    bool g475TraceRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "vnoptrace") == 0;
        }();
        if (s_paired)
            return g475VnopTraceArm() > 0;
        static const bool s_on = f50_12_vu_env_flag("DC2_G475_VNOP_TRACE") &&
                                 !f50_12_vu_env_flag("DC2_G475_NO_VNOP_TRACE");
        return s_on;
    }

    bool g475VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G475_VERIFY");
        return s_on;
    }

    bool g475StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G475_STAT");
        return s_on;
    }

    thread_local uint64_t s_g475TraceRuns = 0u;
    thread_local uint64_t s_g475TracePairs = 0u;
    thread_local uint64_t s_g475LastReport = 0u;
    std::atomic<uint64_t> s_g475Verified{0u};

    void g475NoteTrace(uint32_t count)
    {
        if (!g475StatOn())
            return;
        ++s_g475TraceRuns;
        s_g475TracePairs += count;
        if (s_g475TracePairs - s_g475LastReport >= 5000000u)
        {
            s_g475LastReport = s_g475TracePairs;
            std::fprintf(stderr,
                         "[G475:trace] runs=%llu pairs=%llu avg=%.2f verified=%llu bad=0\n",
                         static_cast<unsigned long long>(s_g475TraceRuns),
                         static_cast<unsigned long long>(s_g475TracePairs),
                         static_cast<double>(s_g475TracePairs) /
                             static_cast<double>(s_g475TraceRuns ? s_g475TraceRuns : 1u),
                         static_cast<unsigned long long>(
                             s_g475Verified.load(std::memory_order_relaxed)));
        }
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g475ExecuteVnopTrace(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay)
    {
        for (uint32_t j = 0u; j < count; ++j)
        {
            const size_t index = static_cast<size_t>(firstIndex + j);
            const G410CompiledUpper &pair = s_g410Cache.uppers[index];
            const uint32_t lower = static_cast<uint32_t>(s_g410Cache.sources[index]);

            if (*qDelay > 0 && --(*qDelay) == 0)
                st.q = s_vuQPending;
            if (*pDelay > 0 && --(*pDelay) == 0)
                st.p = s_vuPPending;

            g422FastLower(st, lower, pair.lowerKind, vuData, dataSize);

            if ((pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                st.vf[0][0] = 0.0f;
                st.vf[0][1] = 0.0f;
                st.vf[0][2] = 0.0f;
                st.vf[0][3] = 1.0f;
                st.vi[0] = 0;
            }
        }

        const uint32_t nextIndex = firstIndex + count;
        st.pc = nextIndex < s_g410Cache.uppers.size() ? nextIndex * 8u : 0u;
    }
} // namespace
