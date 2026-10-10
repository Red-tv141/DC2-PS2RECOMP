// G791 — native ShadowMDT GEOMETRY ORACLE, GS front-door side (diagnostic build only: PS2X_G775_DIAG).
// See dc2_game_override_parts/g791_shadow_native.inc. In DC2_G791_SHADOW=verify the EE keeps every legacy
// shadow face REF, brackets it with two DIRECT 1 marker tags (REGS = 0xC791 | kind<<40 | seq) and publishes
// the bytes its native generator produced for that face. Here, on the thread that parses the GIF stream, the
// PATH1 bytes the VU really XGKICK'd between the two markers are captured and compared BYTE FOR BYTE: every
// generated vertex (XY, Z, F), the primitive tag (type, PRE, NREG/REGS), the RGBAQ words, fan topology, the
// prism / quad order and each quad's ADD / SUB header. The native path never reads any of this.
#include <mutex>
#include <unordered_map>
#include <vector>

namespace g791_oracle {
static std::mutex g_mutex;
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_expect;
static std::atomic<uint64_t> g_faces{0}, g_ok{0}, g_bad{0}, g_missing{0}, g_orphanEnd{0}, g_bytesExp{0}, g_bytesGot{0},
    g_dropped{0};
static std::atomic<uint32_t> g_logged{0};
struct Capture { bool on = false; uint32_t seq = 0; std::vector<uint8_t> bytes; };
static thread_local Capture t_cap;
static std::atomic<bool> g_any{false};
} // namespace g791_oracle

void dc2_g791_expect(uint32_t seq, const uint8_t *bytes, size_t size)
{
    std::lock_guard<std::mutex> lock(g791_oracle::g_mutex);
    if (g791_oracle::g_expect.size() > 65536u)
    {
        g791_oracle::g_dropped.fetch_add(g791_oracle::g_expect.size(), std::memory_order_relaxed);
        g791_oracle::g_expect.clear();
    }
    g791_oracle::g_expect[seq].assign(bytes, bytes + size);
    g791_oracle::g_any.store(true, std::memory_order_relaxed);
}

static inline bool g791OracleCapturing() { return g791_oracle::t_cap.on; }

// Every packet the front door sees while a face is open (the end marker's own 16-byte packet excluded).
static void g791OracleCapture(const uint8_t *data, uint32_t size)
{
    if (size == 16u)
    {
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, data, 8);
        std::memcpy(&hi, data + 8, 8);
        if ((lo & 0x7fffu) == 0u && (hi >> 48) == kG791MarkerMagic)
            return;
    }
    g791_oracle::t_cap.bytes.insert(g791_oracle::t_cap.bytes.end(), data, data + size);
}

static void g791OracleMarker(uint64_t tagHi)
{
    using namespace g791_oracle;
    const uint32_t kind = uint32_t((tagHi >> 40) & 0xffu), seq = uint32_t(tagHi);
    Capture &c = t_cap;
    if (kind == 1u)
    {
        c.on = true;
        c.seq = seq;
        c.bytes.clear();
        return;
    }
    if (kind != 2u)
        return;
    if (!c.on || c.seq != seq)
    {
        g_orphanEnd.fetch_add(1u, std::memory_order_relaxed);
        c.on = false;
        return;
    }
    c.on = false;
    // A front door that hands over several PATH2 windows at once would have appended the end marker's
    // packet (and what follows it): cut the capture at the marker tag itself.
    for (size_t at = c.bytes.size() >= 16u ? (c.bytes.size() - 16u) & ~size_t(15) : 0u; c.bytes.size() >= 16u; at -= 16u)
    {
        uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, c.bytes.data() + at, 8);
        std::memcpy(&hi, c.bytes.data() + at + 8, 8);
        if ((lo & 0x7fffu) == 0u && hi == tagHi)
        {
            c.bytes.resize(at);
            break;
        }
        if (at == 0u)
            break;
    }
    std::vector<uint8_t> want;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_expect.find(seq);
        if (it != g_expect.end())
        {
            want.swap(it->second);
            g_expect.erase(it);
            have = true;
        }
    }
    const uint64_t n = g_faces.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (!have)
        g_missing.fetch_add(1u, std::memory_order_relaxed);
    else
    {
        g_bytesExp.fetch_add(want.size(), std::memory_order_relaxed);
        g_bytesGot.fetch_add(c.bytes.size(), std::memory_order_relaxed);
        if (want == c.bytes)
            g_ok.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            g_bad.fetch_add(1u, std::memory_order_relaxed);
            size_t at = 0;
            while (at < want.size() && at < c.bytes.size() && want[at] == c.bytes[at])
                ++at;
            if (g_logged.fetch_add(1u, std::memory_order_relaxed) < 24u)
            {
                const size_t q = at / 16u;
                auto word = [](const std::vector<uint8_t> &v, size_t qw, int w) {
                    uint32_t x = 0;
                    if ((qw + 1u) * 16u <= v.size())
                        std::memcpy(&x, v.data() + qw * 16u + size_t(w) * 4u, 4);
                    return x;
                };
                std::fprintf(stderr,
                             "[G791:oracle-bad] seq=%u expBytes=%zu gotBytes=%zu firstDiffQw=%zu exp=%08x %08x %08x %08x "
                             "got=%08x %08x %08x %08x\n",
                             seq, want.size(), c.bytes.size(), q, word(want, q, 0), word(want, q, 1), word(want, q, 2),
                             word(want, q, 3), word(c.bytes, q, 0), word(c.bytes, q, 1), word(c.bytes, q, 2),
                             word(c.bytes, q, 3));
                static FILE *dump = std::fopen("logs/g791/oracle_bad.bin", "wb");
                if (dump)
                {
                    const uint32_t hdr[4] = {0x31393747u, seq, uint32_t(want.size()), uint32_t(c.bytes.size())};
                    std::fwrite(hdr, sizeof(hdr), 1, dump);
                    std::fwrite(want.data(), 1, want.size(), dump);
                    std::fwrite(c.bytes.data(), 1, c.bytes.size(), dump);
                    std::fflush(dump);
                }
            }
        }
    }
    if ((n % 2000u) == 0u || n == 200u)
        std::fprintf(stderr,
                     "[G791:oracle] faces=%llu ok=%llu bad=%llu missing=%llu orphanEnd=%llu bytesExp=%llu bytesGot=%llu "
                     "dropped=%llu\n",
                     (unsigned long long)n, (unsigned long long)g_ok.load(), (unsigned long long)g_bad.load(),
                     (unsigned long long)g_missing.load(), (unsigned long long)g_orphanEnd.load(),
                     (unsigned long long)g_bytesExp.load(), (unsigned long long)g_bytesGot.load(),
                     (unsigned long long)g_dropped.load());
}
