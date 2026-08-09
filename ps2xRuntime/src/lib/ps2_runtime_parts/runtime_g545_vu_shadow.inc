// G545: live, default-off CPU-authoritative VU1 -> GPU shadow stream.
//
// The first accepted kick carries a complete seed. Later kicks carry only ordered VIF writes,
// execution parameters, CPU post-state/pipeline, and exact XGKICK outputs. Full post-data is sent
// only at a bounded checkpoint interval. The backend owns and batches the resulting byte blobs;
// this worker never waits for GL and no shadow result can affect guest state or graphics.
class G545VuLiveShadow
{
public:
    G545VuLiveShadow()
    {
        const char *v = std::getenv("DC2_G545_LIVE_SHADOW");
        m_enabled = v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0 &&
                    std::strcmp(v, "false") != 0 && std::strcmp(v, "off") != 0;
        m_dataInterval = envU32("DC2_G545_DATA_CHECK_KICKS", 256u, 1u, 65536u);
    }

    bool begin(const G297KickItem &, const VU1State &state,
               const uint8_t *code, const uint8_t *data)
    {
        if (!m_enabled || m_inFlight)
            return false;
        if (std::getenv("DC2_G545_CPU_TRACE") != nullptr)
            g_g545CpuTraceSequence.store(m_sequence, std::memory_order_relaxed);
        m_seedThisKick = m_needSeed;
        if (m_seedThisKick)
        {
            encodeState(state, m_seedState);
            dc2_g541_vu_pipe_snapshot(m_seedPipe);
            m_seedCode.assign(code, code + PS2_VU1_CODE_SIZE);
            m_seedData.assign(data, data + PS2_VU1_DATA_SIZE);
        }
        m_inFlight = true;
        return true;
    }

    void finish(const G297KickItem &item, const VU1State &state, const uint8_t *postData,
                uint64_t cycles, uint32_t stopReason)
    {
        if (!m_inFlight)
            return;
        m_inFlight = false;

        std::vector<uint8_t> postState;
        std::vector<uint8_t> postPipe;
        encodeState(state, postState);
        dc2_g541_vu_pipe_snapshot(postPipe);
        const bool checkpoint = m_seedThisKick || ((m_sequence % m_dataInterval) == 0u);

        std::vector<uint8_t> payload;
        payload.reserve((m_seedThisKick ? 34044u : 2048u) + item.writeBytes.size());
        const uint32_t seedState = m_seedThisKick ? append(payload, m_seedState) : 0u;
        const uint32_t expectedState = append(payload, postState);
        const uint32_t seedPipe = m_seedThisKick ? append(payload, m_seedPipe) : 0u;
        const uint32_t expectedPipe = append(payload, postPipe);
        const uint32_t seedCode = m_seedThisKick ? append(payload, m_seedCode) : 0u;
        const uint32_t seedData = m_seedThisKick ? append(payload, m_seedData) : 0u;
        const uint32_t checkpointData = checkpoint
            ? append(payload, postData, PS2_VU1_DATA_SIZE) : 0u;

        align4(payload);
        const uint32_t writeDesc = narrow(payload.size());
        for (const G297VuWrite &w : item.writes)
        {
            u32(payload, w.off);
            u32(payload, w.len);
            u32(payload, w.isCode != 0u ? 1u : 0u);
        }
        const uint32_t writeBytes = append(payload, item.writeBytes);

        align4(payload);
        const uint32_t packetDesc = narrow(payload.size());
        uint64_t packetBytes64 = 0ull;
        for (size_t i = 0u; i < item.packets.size(); ++i)
        {
            const uint32_t addr = i < item.packetAddrs.size()
                ? item.packetAddrs[i] : 0xffffffffu;
            u32(payload, addr);
            u32(payload, narrow(item.packets[i].size()));
            packetBytes64 += item.packets[i].size();
        }
        align4(payload);
        const uint32_t packetBytes = narrow(payload.size());
        for (const auto &packet : item.packets)
            bytes(payload, packet.data(), packet.size());
        align4(payload);

        if (postState.size() != 632u || postPipe.size() != 644u ||
            (m_seedThisKick && (m_seedState.size() != 632u || m_seedPipe.size() != 644u ||
                               m_seedCode.size() != PS2_VU1_CODE_SIZE ||
                               m_seedData.size() != PS2_VU1_DATA_SIZE)) ||
            payload.size() > kMaxBlobBytes - 128u || item.writes.size() > 0xffffffffull ||
            item.writeBytes.size() > 0xffffffffull || item.packets.size() > 0xffffffffull ||
            packetBytes64 > 0xffffffffull)
        {
            disable("serialization bound/state schema failure");
            return;
        }

        uint32_t cmd[32]{};
        cmd[0] = 0x35435556u; // little-endian "VUC5"
        cmd[1] = 1u;
        cmd[2] = 32u;
        cmd[3] = (item.isResume ? 1u : 0u) | (m_seedThisKick ? 2u : 0u) |
                 (checkpoint ? 4u : 0u);
        cmd[4] = static_cast<uint32_t>(m_sequence);
        cmd[5] = static_cast<uint32_t>(m_sequence >> 32u);
        cmd[6] = item.startPC;
        cmd[7] = item.itop;
        cmd[8] = item.top;
        cmd[9] = item.budget;
        cmd[10] = static_cast<uint32_t>(cycles);
        cmd[11] = static_cast<uint32_t>(cycles >> 32u);
        cmd[12] = stopReason;
        cmd[13] = 0u; // backend fills absolute payload base
        cmd[14] = narrow(payload.size());
        cmd[15] = seedState;
        cmd[16] = expectedState;
        cmd[17] = seedPipe;
        cmd[18] = expectedPipe;
        cmd[19] = seedCode;
        cmd[20] = seedData;
        cmd[21] = checkpointData;
        cmd[22] = writeDesc;
        cmd[23] = narrow(item.writes.size());
        cmd[24] = writeBytes;
        cmd[25] = narrow(item.writeBytes.size());
        cmd[26] = packetDesc;
        cmd[27] = narrow(item.packets.size());
        cmd[28] = packetBytes;
        cmd[29] = static_cast<uint32_t>(packetBytes64);
        cmd[30] = m_dataInterval;
        cmd[31] = narrow(payload.size());

        std::vector<uint8_t> blob;
        blob.reserve(128u + payload.size());
        for (uint32_t word : cmd)
            u32(blob, word);
        bytes(blob, payload.data(), payload.size());

        const uint64_t submittedSequence = m_sequence++;
        if (!g545_backend_live_submit(std::move(blob)))
        {
            m_needSeed = true;
            disable("backend rejected asynchronous command");
            return;
        }
        m_needSeed = false;
        if (!m_announced)
        {
            m_announced = true;
            std::fprintf(stderr,
                         "[G545:live] worker stream ACTIVE seedSeq=%llu dataCheckpoint=%u CPU-authoritative=1\n",
                         static_cast<unsigned long long>(submittedSequence), m_dataInterval);
        }
    }

private:
    static constexpr size_t kMaxBlobBytes = 2u * 1024u * 1024u;

    static uint32_t envU32(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
    {
        const char *value = std::getenv(name);
        if (value == nullptr || value[0] == '\0')
            return fallback;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value)
            return fallback;
        return static_cast<uint32_t>(std::max<unsigned long>(lo,
            std::min<unsigned long>(hi, parsed)));
    }

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
    static uint32_t append(std::vector<uint8_t> &out, const std::vector<uint8_t> &src)
    {
        return append(out, src.data(), src.size());
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
    void disable(const char *why)
    {
        if (m_enabled)
            std::fprintf(stderr, "[G545:live] FAIL %s; shadow disabled, CPU path unchanged\n", why);
        m_enabled = false;
        m_inFlight = false;
    }

    bool m_enabled = false;
    bool m_inFlight = false;
    bool m_needSeed = true;
    bool m_seedThisKick = false;
    bool m_announced = false;
    uint32_t m_dataInterval = 256u;
    uint64_t m_sequence = 0ull;
    std::vector<uint8_t> m_seedState, m_seedPipe, m_seedCode, m_seedData;
};
