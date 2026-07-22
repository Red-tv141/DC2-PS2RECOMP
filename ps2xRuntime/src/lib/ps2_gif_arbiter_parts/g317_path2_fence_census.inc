// G317 (diagnosis only, default off): identify FIFO-safe Path-2 parser runs without
// changing the dispatch stream. VIF-side fences publish an epoch; QueueItems retain
// that epoch, and the GS worker closes a candidate run on every explicit fence,
// non-Path-2 packet, IMAGE transfer, DIRECTHL packet, apply, or frame boundary.
//
// Keep these values synchronized with the small declaration block in
// vif1_probes_and_helpers.inc. They are runtime-private and deliberately avoid a
// public header: project rules prohibit broad header churn for phase probes.
enum G317VifFenceReason : uint32_t
{
    kG317VifFlush  = 1u << 0u,
    kG317VifFlusha = 1u << 1u,
    kG317VifMscal  = 1u << 2u,
    kG317VifMscalf = 1u << 3u,
    kG317VifMscnt  = 1u << 4u,
};

namespace
{
    bool g317FenceCensusOn()
    {
        static const bool s_on = f50_12_env_flag("DC2_G317_FENCE_CENSUS");
        return s_on;
    }

    std::atomic<uint64_t> g_g317VifFenceEpoch{0u};
    std::atomic<uint32_t> g_g317VifFenceReasons{0u};

    enum G317BarrierIndex : uint32_t
    {
        kG317BarrierFlush = 0u,
        kG317BarrierFlusha,
        kG317BarrierMscal,
        kG317BarrierMscalf,
        kG317BarrierMscnt,
        kG317BarrierPath1,
        kG317BarrierPath3,
        kG317BarrierImage,
        kG317BarrierDirectHl,
        kG317BarrierUnknown,
        kG317BarrierApply,
        kG317BarrierFrame,
        kG317BarrierCount,
    };

    enum class G317Path2Class : uint8_t
    {
        Eligible,
        Image,
        DirectHl,
        Unknown,
    };

    struct G317Path2ScanState
    {
        uint64_t pendingImageQwc = 0u;
        bool known = true;
    };

    struct G317CensusStats
    {
        uint64_t frames = 0u;
        uint64_t runs = 0u;
        uint64_t eligiblePackets = 0u;
        uint64_t eligibleBytes = 0u;
        uint64_t eligibleWindows = 0u;
        uint64_t readyWindows = 0u;
        uint64_t explicitEpochs = 0u;
        uint64_t maxRun = 0u;
        uint64_t runBins[7]{}; // 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+
        uint64_t barriers[kG317BarrierCount]{};
    };

    G317Path2Class g317ClassifyPath2Packet(const GifArbiterPacket &pkt,
                                           G317Path2ScanState &state)
    {
        if (!state.known)
            return G317Path2Class::Unknown;
        if (pkt.data.empty() || (pkt.data.size() & 15u) != 0u)
        {
            state.known = false;
            return G317Path2Class::Unknown;
        }

        const uint64_t packetQwc = pkt.data.size() / 16u;
        uint64_t qword = 0u;
        bool hasImage = false;

        if (state.pendingImageQwc != 0u)
        {
            hasImage = true;
            const uint64_t consumed = std::min<uint64_t>(state.pendingImageQwc, packetQwc);
            state.pendingImageQwc -= consumed;
            qword += consumed;
            if (qword == packetQwc)
                return G317Path2Class::Image;
        }

        while (qword < packetQwc)
        {
            const uint64_t byteOffset = qword * 16u;
            if (byteOffset + 16u > pkt.data.size())
            {
                state.known = false;
                return G317Path2Class::Unknown;
            }

            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, pkt.data.data() + byteOffset, sizeof(tagLo));
            const uint64_t nloop = tagLo & 0x7FFFu;
            const uint32_t flg = static_cast<uint32_t>((tagLo >> 58u) & 0x3u);
            uint64_t nreg = (tagLo >> 60u) & 0xFu;
            if (nreg == 0u)
                nreg = 16u;

            ++qword;
            uint64_t payloadQwc = 0u;
            if (flg == 0u)
                payloadQwc = nloop * nreg;
            else if (flg == 1u)
                payloadQwc = (nloop * nreg + 1u) / 2u;
            else
            {
                payloadQwc = nloop;
                hasImage = true;
            }

            const uint64_t availableQwc = packetQwc - qword;
            if (payloadQwc > availableQwc)
            {
                if (flg < 2u)
                {
                    state.known = false;
                    return G317Path2Class::Unknown;
                }
                state.pendingImageQwc = payloadQwc - availableQwc;
                qword = packetQwc;
                break;
            }
            qword += payloadQwc;
        }

        if (hasImage)
            return G317Path2Class::Image;
        if (pkt.path2DirectHl)
            return G317Path2Class::DirectHl;
        return G317Path2Class::Eligible;
    }

    uint32_t g317RunBin(uint64_t packets)
    {
        if (packets <= 1u) return 0u;
        if (packets <= 3u) return 1u;
        if (packets <= 7u) return 2u;
        if (packets <= 15u) return 3u;
        if (packets <= 31u) return 4u;
        if (packets <= 63u) return 5u;
        return 6u;
    }
}

// Called from the VIF1 command parser before commands that delimit or can create
// GIF-path work. Default-off and cached: normal execution performs no atomic work.
void g317_note_gif_fence(uint32_t reason)
{
    if (!g317FenceCensusOn())
        return;
    g_g317VifFenceReasons.fetch_or(reason, std::memory_order_release);
    g_g317VifFenceEpoch.fetch_add(1u, std::memory_order_release);
}
