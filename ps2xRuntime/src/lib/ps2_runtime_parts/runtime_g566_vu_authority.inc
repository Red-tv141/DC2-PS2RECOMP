// G566: exact, default-off GPU-authoritative VU1 command producer.
//
// Unlike G545 shadow commands, these blobs are built before CPU VU execution and contain only the
// persistent seed, ordered VIF writes, and execution parameters. The GPU publishes generated
// XGKICK bytes through G566VuAuthorityResult; no CPU post-state or packet oracle is involved.
inline bool g566VuAuthorityRequested()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_G566_GPU_VU");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0 &&
               std::strcmp(v, "false") != 0 && std::strcmp(v, "off") != 0;
    }();
    return on;
}

class G566VuAuthority
{
public:
    bool submit(const G297KickItem &item, const VU1State &state,
                const uint8_t *code, const uint8_t *data,
                const std::shared_ptr<G566VuAuthorityResult> &result)
    {
        if (!g566VuAuthorityRequested() || !result)
            return false;

        const bool seed = m_needSeed;
        std::vector<uint8_t> payload;
        payload.reserve(seed ? 34000u + item.writeBytes.size()
                             : 128u + item.writeBytes.size());

        uint32_t seedState = 0u, seedPipe = 0u, seedCode = 0u, seedData = 0u;
        if (seed)
        {
            std::vector<uint8_t> encodedState;
            std::vector<uint8_t> encodedPipe;
            encodeState(state, encodedState);
            dc2_g541_vu_pipe_snapshot(encodedPipe);
            canonicalizeSeedHistoryPipe(encodedPipe);
            if (encodedState.size() != 632u || encodedPipe.size() != 644u)
            {
                publishFailure(result, "seed state schema failure");
                return true;
            }
            seedState = append(payload, encodedState.data(), encodedState.size());
            seedPipe = append(payload, encodedPipe.data(), encodedPipe.size());
            seedCode = append(payload, code, PS2_VU1_CODE_SIZE);
            seedData = append(payload, data, PS2_VU1_DATA_SIZE);
        }

        align4(payload);
        const uint32_t writeDesc = narrow(payload.size());
        for (const G297VuWrite &w : item.writes)
        {
            u32(payload, w.off);
            u32(payload, w.len);
            u32(payload, w.isCode != 0u ? 1u : 0u);
        }
        const uint32_t writeBytes = append(payload, item.writeBytes.data(), item.writeBytes.size());
        align4(payload);
        if (payload.empty())
            u32(payload, 0u); // keep the packed payload image non-empty for write-free resume kicks

        if (payload.size() > kMaxBlobBytes - 128u || item.writes.size() > 0xffffffffull ||
            item.writeBytes.size() > 0xffffffffull)
        {
            publishFailure(result, "command serialization bound failure");
            return true;
        }

        result->sequence = m_sequence;
        uint32_t cmd[32]{};
        cmd[0] = 0x36435556u; // little-endian "VUC6"
        cmd[1] = 1u;
        cmd[2] = 32u;
        cmd[3] = (item.isResume ? 1u : 0u) | (seed ? 2u : 0u) | 8u;
        cmd[4] = static_cast<uint32_t>(m_sequence);
        cmd[5] = static_cast<uint32_t>(m_sequence >> 32u);
        cmd[6] = item.startPC;
        cmd[7] = item.itop;
        cmd[8] = item.top;
        cmd[9] = item.budget;
        cmd[13] = 0u; // backend fills absolute payload base
        cmd[14] = narrow(payload.size());
        cmd[15] = seedState;
        cmd[17] = seedPipe;
        cmd[19] = seedCode;
        cmd[20] = seedData;
        cmd[22] = writeDesc;
        cmd[23] = narrow(item.writes.size());
        cmd[24] = writeBytes;
        cmd[25] = narrow(item.writeBytes.size());
        cmd[31] = narrow(payload.size());

        std::vector<uint8_t> blob;
        blob.reserve(128u + payload.size());
        for (uint32_t word : cmd)
            u32(blob, word);
        bytes(blob, payload.data(), payload.size());

        const uint64_t sequence = m_sequence++;
        if (!g566_backend_authority_submit(std::move(blob), result))
        {
            publishFailure(result, "backend rejected command");
            return true;
        }
        m_needSeed = false;
        if (!m_announced)
        {
            m_announced = true;
            std::fprintf(stderr,
                         "[G566:authority] ACTIVE seedSeq=%llu GPU state/RAM + GPU XGKICK; CPU VU skipped\n",
                         static_cast<unsigned long long>(sequence));
        }
        return true;
    }

private:
    static constexpr size_t kMaxBlobBytes = 2u * 1024u * 1024u;

    static uint32_t narrow(size_t n) { return static_cast<uint32_t>(n); }
    static void align4(std::vector<uint8_t> &out)
    {
        while ((out.size() & 3u) != 0u)
            out.push_back(0u);
    }
    static void bytes(std::vector<uint8_t> &out, const void *src, size_t n)
    {
        if (n == 0u)
            return;
        const auto *p = static_cast<const uint8_t *>(src);
        out.insert(out.end(), p, p + n);
    }
    static void u32(std::vector<uint8_t> &out, uint32_t v)
    {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8u));
        out.push_back(static_cast<uint8_t>(v >> 16u));
        out.push_back(static_cast<uint8_t>(v >> 24u));
    }
    static uint32_t append(std::vector<uint8_t> &out, const void *src, size_t n)
    {
        align4(out);
        const uint32_t at = narrow(out.size());
        bytes(out, src, n);
        return at;
    }
    static void encodeState(const VU1State &s, std::vector<uint8_t> &out)
    {
        out.clear();
        out.reserve(632u);
        const auto f32 = [&](float v) {
            uint32_t bits = 0u;
            std::memcpy(&bits, &v, sizeof(bits));
            u32(out, bits);
        };
        for (const auto &vf : s.vf) for (float v : vf) f32(v);
        for (int32_t v : s.vi) u32(out, static_cast<uint32_t>(v));
        for (float v : s.acc) f32(v);
        f32(s.q); f32(s.p); f32(s.i);
        u32(out, s.pc); u32(out, s.mac); u32(out, s.clip); u32(out, s.status);
        u32(out, s.ebit ? 1u : 0u); u32(out, s.itop); u32(out, s.xitop);
    }
    static void canonicalizeSeedHistoryPipe(std::vector<uint8_t> &pipe)
    {
        if (pipe.size() != 644u)
            return;
        for (uint32_t slot = 0u; slot < 8u; ++slot)
        {
            uint32_t dest = 0u;
            std::memcpy(&dest, pipe.data() + (138u + slot) * 4u, sizeof(dest));
            for (uint32_t lane = 0u; lane < 4u; ++lane)
                if ((dest & (8u >> lane)) == 0u)
                {
                    uint32_t value = 0u;
                    uint8_t *const at = pipe.data() + (82u + slot * 4u + lane) * 4u;
                    std::memcpy(&value, at, sizeof(value));
                    if ((value & 0x7f800000u) == 0u && (value & 0x007fffffu) != 0u)
                        std::memset(at, 0, sizeof(value));
                }
        }
    }
    static void publishFailure(const std::shared_ptr<G566VuAuthorityResult> &result,
                               const char *why)
    {
        {
            std::lock_guard<std::mutex> lk(result->mutex);
            result->ok = false;
            result->ready.store(true, std::memory_order_release);
        }
        result->cv.notify_all();
        static std::atomic<uint32_t> s_reports{0u};
        if (s_reports.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr, "[G566:authority] FAIL %s\n", why);
    }

    bool m_needSeed = true;
    bool m_announced = false;
    uint64_t m_sequence = 0ull;
};
