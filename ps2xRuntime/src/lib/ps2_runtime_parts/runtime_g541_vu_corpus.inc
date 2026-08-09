// G541: bounded CPU-reference corpus at the G297 kick boundary.
//
// Each record is self-contained: explicit VU register state + hidden pipeline state, full code and
// data RAM before execution, full data RAM after execution, ordered EE->VU writes, kick parameters,
// stop reason, and ordered XGKICK (source address, bytes) outputs. Host padding is never serialized.
// Default OFF. The CPU interpreter remains authoritative and this writer changes no guest byte.
class G541VuCorpus
{
public:
    G541VuCorpus()
    {
        const char *enabled = std::getenv("DC2_G541_VU_CORPUS");
        m_enabled = enabled != nullptr && enabled[0] != '\0' &&
                    std::strcmp(enabled, "0") != 0 && std::strcmp(enabled, "false") != 0 &&
                    std::strcmp(enabled, "off") != 0;
        m_limit = envU32("DC2_G541_VU_CORPUS_KICKS", 256u, 1u, 4096u);
        m_skip = envU32("DC2_G541_VU_CORPUS_SKIP", 0u, 0u, 10000000u);
    }

    bool begin(const G297KickItem &, const VU1State &state,
               const uint8_t *code, const uint8_t *data)
    {
        if (!m_enabled || m_records >= m_limit || m_inFlight)
            return false;
        if (m_seen++ < m_skip)
            return false;
        if (!openOnce())
            return false;

        encodeState(state, m_preState);
        dc2_g541_vu_pipe_snapshot(m_prePipe);
        m_preCode.assign(code, code + PS2_VU1_CODE_SIZE);
        m_preData.assign(data, data + PS2_VU1_DATA_SIZE);
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

        m_payload.clear();
        m_payload.reserve(m_preState.size() + postState.size() + m_prePipe.size() +
                          postPipe.size() + m_preCode.size() + m_preData.size() +
                          PS2_VU1_DATA_SIZE + item.writeBytes.size() + 4096u);
        bytes(m_payload, m_preState.data(), m_preState.size());
        bytes(m_payload, postState.data(), postState.size());
        bytes(m_payload, m_prePipe.data(), m_prePipe.size());
        bytes(m_payload, postPipe.data(), postPipe.size());
        bytes(m_payload, m_preCode.data(), m_preCode.size());
        bytes(m_payload, m_preData.data(), m_preData.size());
        bytes(m_payload, postData, PS2_VU1_DATA_SIZE);
        for (const G297VuWrite &w : item.writes)
        {
            u32(m_payload, w.off);
            u32(m_payload, w.len);
            u32(m_payload, w.isCode != 0u ? 1u : 0u);
        }
        bytes(m_payload, item.writeBytes.data(), item.writeBytes.size());

        uint64_t packetBytes64 = 0ull;
        for (size_t i = 0; i < item.packets.size(); ++i)
        {
            const uint32_t addr = i < item.packetAddrs.size() ? item.packetAddrs[i] : 0xffffffffu;
            u32(m_payload, addr);
            u32(m_payload, static_cast<uint32_t>(item.packets[i].size()));
            packetBytes64 += item.packets[i].size();
        }
        for (const auto &packet : item.packets)
            bytes(m_payload, packet.data(), packet.size());

        std::vector<uint8_t> header;
        static const uint8_t kRecordMagic[8] = {'G','5','4','1','R','E','C','1'};
        bytes(header, kRecordMagic, sizeof(kRecordMagic));
        u32(header, 1u);
        const size_t headerBytesOffset = header.size();
        u32(header, 0u);
        u64(header, m_records);
        u64(header, m_payload.size());
        u64(header, fnv1a(m_payload.data(), m_payload.size()));
        u64(header, cycles);
        u32(header, item.isResume ? 1u : 0u);
        u32(header, item.startPC);
        u32(header, item.itop);
        u32(header, item.top);
        u32(header, item.budget);
        u32(header, stopReason);
        u32(header, static_cast<uint32_t>(item.writes.size()));
        u32(header, static_cast<uint32_t>(item.writeBytes.size()));
        u32(header, static_cast<uint32_t>(item.packets.size()));
        u32(header, static_cast<uint32_t>(packetBytes64));
        u32(header, static_cast<uint32_t>(m_preState.size()));
        u32(header, static_cast<uint32_t>(postState.size()));
        u32(header, static_cast<uint32_t>(m_prePipe.size()));
        u32(header, static_cast<uint32_t>(postPipe.size()));
        u32(header, static_cast<uint32_t>(m_preCode.size()));
        u32(header, static_cast<uint32_t>(m_preData.size()));
        u32(header, PS2_VU1_DATA_SIZE);
        patchU32(header, headerBytesOffset, static_cast<uint32_t>(header.size()));

        m_out.write(reinterpret_cast<const char *>(header.data()),
                    static_cast<std::streamsize>(header.size()));
        m_out.write(reinterpret_cast<const char *>(m_payload.data()),
                    static_cast<std::streamsize>(m_payload.size()));
        ++m_records;
        if (!m_out.good())
        {
            std::fprintf(stderr, "[G541:corpus] FAIL write path='%s' record=%llu\n",
                         m_path.c_str(), static_cast<unsigned long long>(m_records - 1u));
            m_enabled = false;
            m_out.close();
        }
        else if (m_records == 1u)
        {
            std::fprintf(stderr,
                         "[G541:corpus] recording path='%s' limit=%u skip=%u state=%zu pipe=%zu\n",
                         m_path.c_str(), m_limit, m_skip, m_preState.size(), m_prePipe.size());
        }

        if (m_out.good() && ((m_records & 15u) == 0u || m_records == m_limit))
            m_out.flush();
        if (m_records == m_limit)
        {
            std::fprintf(stderr,
                         "[G541:corpus] COMPLETE records=%llu path='%s' lastPackets=%zu\n",
                         static_cast<unsigned long long>(m_records), m_path.c_str(),
                         item.packets.size());
            m_out.close();
        }
    }

private:
    static uint32_t envU32(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
    {
        const char *value = std::getenv(name);
        if (value == nullptr || value[0] == '\0')
            return fallback;
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        if (end == value)
            return fallback;
        return static_cast<uint32_t>(std::max<unsigned long>(lo, std::min<unsigned long>(hi, parsed)));
    }

    bool openOnce()
    {
        if (m_openTried)
            return m_out.is_open();
        m_openTried = true;
        const char *path = std::getenv("DC2_G541_VU_CORPUS_PATH");
        m_path = (path != nullptr && path[0] != '\0') ? path : "captures/g541_vu1_corpus.bin";
        m_out.open(m_path, std::ios::binary | std::ios::trunc);
        if (!m_out)
        {
            std::fprintf(stderr, "[G541:corpus] FAIL open path='%s'\n", m_path.c_str());
            m_enabled = false;
            return false;
        }
        std::vector<uint8_t> fileHeader;
        static const uint8_t kFileMagic[8] = {'D','C','2','V','U','1','K','\0'};
        bytes(fileHeader, kFileMagic, sizeof(kFileMagic));
        u32(fileHeader, 1u);
        u32(fileHeader, 0x01020304u);
        u32(fileHeader, PS2_VU1_CODE_SIZE);
        u32(fileHeader, PS2_VU1_DATA_SIZE);
        u32(fileHeader, 632u);
        u32(fileHeader, 32u);
        m_out.write(reinterpret_cast<const char *>(fileHeader.data()),
                    static_cast<std::streamsize>(fileHeader.size()));
        return m_out.good();
    }

    static void bytes(std::vector<uint8_t> &out, const void *src, size_t n)
    {
        if (n == 0u)
            return;
        const uint8_t *p = static_cast<const uint8_t *>(src);
        out.insert(out.end(), p, p + n);
    }
    static void u32(std::vector<uint8_t> &out, uint32_t v)
    {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8u));
        out.push_back(static_cast<uint8_t>(v >> 16u));
        out.push_back(static_cast<uint8_t>(v >> 24u));
    }
    static void u64(std::vector<uint8_t> &out, uint64_t v)
    {
        for (unsigned i = 0; i < 8u; ++i)
            out.push_back(static_cast<uint8_t>(v >> (i * 8u)));
    }
    static void patchU32(std::vector<uint8_t> &out, size_t off, uint32_t v)
    {
        for (unsigned i = 0; i < 4u; ++i)
            out[off + i] = static_cast<uint8_t>(v >> (i * 8u));
    }
    static uint64_t fnv1a(const uint8_t *p, size_t n)
    {
        uint64_t h = 14695981039346656037ull;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
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

    bool m_enabled = false;
    bool m_openTried = false;
    bool m_inFlight = false;
    uint32_t m_limit = 256u;
    uint32_t m_skip = 0u;
    uint64_t m_seen = 0u;
    uint64_t m_records = 0u;
    std::string m_path;
    std::ofstream m_out;
    std::vector<uint8_t> m_preState, m_prePipe, m_preCode, m_preData, m_payload;
};
