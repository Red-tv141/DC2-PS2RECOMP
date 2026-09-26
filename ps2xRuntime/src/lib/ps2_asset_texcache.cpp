#include "ps2_asset_texcache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace dc2::assetcache
{
namespace
{
constexpr uint32_t Version = 2;
constexpr size_t VramSize = 4u * 1024u * 1024u;
constexpr uint64_t Basis = 14695981039346656037ull;
uint32_t u32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p + 4)) << 32); }
uint64_t hash(const uint8_t* p, size_t n, uint64_t h = Basis)
{
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
bool power2(uint32_t n) { return n && !(n & (n - 1)); }
bool flag(const char* key)
{
    const char* p = std::getenv(key);
    return p && std::strcmp(p, "1") == 0;
}
uint64_t descriptorHash(const Descriptor& d)
{
    uint64_t h = Basis;
    for (uint32_t v : {d.width, d.height, d.tbw, d.psm})
        for (unsigned s = 0; s < 32; s += 8) h = (h ^ ((v >> s) & 255)) * 1099511628211ull;
    return h;
}
struct Span { uint32_t plane, offset, size; const uint8_t* bytes; };
struct Entry
{
    Descriptor desc;
    std::vector<Span> spans;
    Image original, replacement;
    const uint8_t* record = nullptr;
    size_t recordBytes = 0;
    // atomic_ref permits vector relocation while constructing the unpublished index.
    mutable uint8_t validated = 0;
};
// IEEE CRC32, matching Python zlib.crc32. Slicing by eight makes one-time record
// verification cheap without requiring SSE4.2 or architecture-specific intrinsics.
uint32_t crc32(const uint8_t* p, size_t size, uint32_t crc = 0)
{
    static const auto table = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
            t[0][i] = c;
        }
        for (size_t j = 1; j < 8; ++j) for (size_t i = 0; i < 256; ++i)
            t[j][i] = (t[j-1][i] >> 8) ^ t[0][t[j-1][i] & 255];
        return t;
    }();
    crc = ~crc;
    while (size >= 8)
    {
        const uint32_t a = u32(p) ^ crc, b = u32(p + 4);
        crc = table[7][a & 255] ^ table[6][(a >> 8) & 255] ^
              table[5][(a >> 16) & 255] ^ table[4][a >> 24] ^
              table[3][b & 255] ^ table[2][(b >> 8) & 255] ^
              table[1][(b >> 16) & 255] ^ table[0][b >> 24];
        p += 8; size -= 8;
    }
    while (size--) crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 255];
    return ~crc;
}
bool validate(const Entry& e)
{
    std::atomic_ref<uint8_t> state(e.validated);
    const uint8_t old = state.load(std::memory_order_acquire);
    if (old) return old == 1;
    const uint32_t sum = crc32(e.record + 68, e.recordBytes - 68, crc32(e.record, 64));
    const bool ok = sum == u32(e.record + 64);
    state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}
std::atomic<uint64_t> hits{0}, misses{0}, texels{0}, replaced{0}, badPixels{0};
std::atomic<uint64_t> uploads{0}, uploadBytes{0}, residentHits{0}, avoidedBytes{0};
void report()
{
    std::fprintf(stderr, "[G764:texcache] hits=%llu misses=%llu pixels=%llu replacements=%llu bad=%llu uploads=%llu uploadBytes=%llu resident=%llu avoidedBytes=%llu\n",
        (unsigned long long)hits.load(), (unsigned long long)misses.load(),
        (unsigned long long)texels.load(), (unsigned long long)replaced.load(),
        (unsigned long long)badPixels.load(), (unsigned long long)uploads.load(),
        (unsigned long long)uploadBytes.load(), (unsigned long long)residentHits.load(),
        (unsigned long long)avoidedBytes.load());
}
}

struct Cache::Impl
{
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    std::vector<uint8_t> storage;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
#endif
    std::vector<Entry> entries;
    std::unordered_multimap<uint64_t, size_t> index;
    ~Impl()
    {
#ifdef _WIN32
        if (mapping) { UnmapViewOfFile(data); CloseHandle(mapping); }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#endif
    }
    bool map(const std::string& path)
    {
#ifdef _WIN32
        file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER length{};
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length) ||
            length.QuadPart < 64 || uint64_t(length.QuadPart) > 4ull * 1024 * 1024 * 1024) return false;
        bytes = size_t(length.QuadPart);
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
#else
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        const auto length = f.tellg();
        if (!f || length < 64 || uint64_t(length) > 4ull * 1024 * 1024 * 1024) return false;
        storage.resize(size_t(length)); f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(storage.data()), length)) return false;
        data = storage.data(); bytes = storage.size(); return true;
#endif
    }
};

Cache::Cache() : m(std::make_unique<Impl>()) {}
Cache::~Cache() = default;
size_t Cache::size() const { return m->entries.size(); }

bool Cache::open(const std::string& path, std::string* error)
{
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    auto next = std::make_unique<Impl>();
    if (!next->map(path)) return fail("cannot map cache");
    const auto* p = next->data;
    if (std::memcmp(p, "DC2ATEX\0", 8) || u32(p + 8) != Version || u32(p + 12) != 64 ||
        u64(p + 16) != next->bytes || u32(p + 28) != 0) return fail("unsupported header");
    const uint32_t count = u32(p + 24);
    if (count > 100000) return fail("invalid count");
    for (size_t i = 40; i < 64; ++i) if (p[i]) return fail("unknown header flags");
    size_t pos = 64;
    uint64_t metadataHash = Basis;
    next->entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (next->bytes - pos < 80) return fail("truncated entry");
        const uint8_t* h = p + pos;
        metadataHash = hash(h, 80, metadataHash);
        const uint32_t recordBytes = u32(h), spanCount = u32(h + 20);
        Entry e; e.desc = {u32(h + 4), u32(h + 8), u32(h + 12), u32(h + 16)};
        const auto& d = e.desc;
        const uint32_t rw = u32(h + 24), rh = u32(h + 28);
        if (recordBytes < 80 || recordBytes > next->bytes - pos || (recordBytes & 3) ||
            !power2(d.width) || !power2(d.height) || d.width < 32 || d.height < 16 ||
            d.width > 1024 || d.height > 1024 || d.tbw != std::max(1u, d.width / 64) ||
            (d.psm != 0 && d.psm != 0x13 && d.psm != 0x14) ||
            spanCount == 0 || spanCount > 32768 || rw > 8192 || rh > 8192 ||
            bool(rw) != bool(rh)) return fail("invalid entry metadata");
        // Source asset SHA256 is retained in the record and audit manifest. It is
        // not used as an excuse to skip the exact byte comparisons below.
        bool sourceId = false;
        for (size_t j = 32; j < 64; ++j) sourceId |= h[j] != 0;
        if (!sourceId) return fail("entry has no source identity");
        for (size_t j = 68; j < 80; ++j) if (h[j]) return fail("unknown entry flags");
        e.record = h; e.recordBytes = recordBytes;
        const size_t end = pos + recordBytes;
        size_t at = pos + 80;
        uint32_t lastPlane = 0, lastEnd = 0;
        bool havePlane[2] = {};
        for (uint32_t j = 0; j < spanCount; ++j)
        {
            if (end - at < 12) return fail("truncated span");
            metadataHash = hash(p + at, 12, metadataHash);
            Span s{u32(p + at), u32(p + at + 4), u32(p + at + 8), nullptr}; at += 12;
            if (s.plane > 1 || (d.psm == 0 && s.plane) || !s.size || s.offset >= VramSize ||
                s.size > VramSize - s.offset || s.size > end - at ||
                s.plane < lastPlane || (s.plane == lastPlane && s.offset < lastEnd))
                return fail("invalid span range");
            if (!havePlane[s.plane] && (s.offset != 0 || s.size < 16))
                return fail("missing probe bytes");
            havePlane[s.plane] = true; lastPlane = s.plane; lastEnd = s.offset + s.size;
            s.bytes = p + at; at += s.size;
            const size_t aligned = (at + 3) & ~size_t(3);
            if (aligned > end) return fail("invalid span padding");
            while (at < aligned) if (p[at++]) return fail("nonzero span padding");
            e.spans.push_back(s);
        }
        if (!havePlane[0] || (d.psm != 0 && !havePlane[1])) return fail("missing source plane");
        const uint64_t originalBytes = uint64_t(d.width) * d.height * 4;
        const uint64_t replacementBytes = uint64_t(rw) * rh * 4;
        if (originalBytes + replacementBytes != end - at) return fail("invalid image length");
        e.original = {reinterpret_cast<const uint32_t*>(p + at), d.width, d.height, false};
        if (rw) e.replacement = {reinterpret_cast<const uint32_t*>(p + at + originalBytes), rw, rh, true};
        e.original.residentKey = (1ull << 61) | (uint64_t(i) * 2 + 1);
        e.replacement.residentKey = (1ull << 61) | (uint64_t(i) * 2 + 2);
        e.original.originalPixels = e.replacement.originalPixels = e.original.pixels;
        uint64_t key = descriptorHash(d);
        key = hash(e.spans.front().bytes, 16, key);
        if (d.psm != 0)
            for (const auto& s : e.spans) if (s.plane == 1) { key = hash(s.bytes, 16, key); break; }
        next->index.emplace(key, next->entries.size());
        next->entries.push_back(std::move(e)); pos = end;
    }
    if (pos != next->bytes) return fail("trailing cache bytes");
    if (metadataHash != u64(p + 32)) return fail("metadata checksum mismatch");
    m = std::move(next);
    return true;
}

Image Cache::find(const Descriptor& d, const uint8_t* vram, size_t size,
                  uint32_t tbp, uint32_t cbp, bool replacement) const
{
    if (!vram || size != VramSize || tbp >= VramSize / 256 || cbp >= VramSize / 256) return {};
    const uint32_t bases[2] = {tbp * 256u, cbp * 256u};
    uint64_t key = hash(vram + bases[0], 16, descriptorHash(d));
    if (d.psm != 0) key = hash(vram + bases[1], 16, key);
    const auto range = m->index.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
    {
        const auto& e = m->entries[it->second];
        if (!(d == e.desc)) continue;
        bool equal = true;
        for (const auto& s : e.spans)
        {
            // Fail closed on wrap/alias; no partial matches or hash-only hits.
            const uint64_t offset = uint64_t(bases[s.plane]) + s.offset;
            if (offset + s.size > size || std::memcmp(vram + offset, s.bytes, s.size))
            { equal = false; break; }
        }
        if (equal && validate(e)) return replacement && e.replacement ? e.replacement : e.original;
    }
    return {};
}

const Cache* runtimeCache()
{
    static const Cache* cache = []() -> const Cache* {
        const char* path = std::getenv("DC2_ASSET_TEXCACHE");
        if (flag("DC2_G764_NO_TEXCACHE")) return nullptr;
        // Installer/offline compiler places the original cache here. Absence
        // is normal on a first launch: keep the exact runtime texture path.
        if (!path || !*path)
        {
            path = "Mods/TexCache/original.dc2tc";
            std::ifstream probe(path, std::ios::binary);
            if (!probe) return nullptr;
        }
        auto candidate = std::make_unique<Cache>(); std::string error;
        if (!candidate->open(path, &error))
        { std::fprintf(stderr, "[G764:cache] fallback: %s\n", error.c_str()); return nullptr; }
        std::fprintf(stderr, "[G764:cache] loaded=%zu version=%u\n", candidate->size(), Version);
        // Deliberately process lifetime: queued upload pointers may outlive renderer state.
        return candidate.release();
    }();
    return cache;
}
bool replacementsEnabled() { static const bool on = flag("DC2_ASSET_REPLACEMENTS"); return on; }
bool verifyEnabled() { static const bool on = flag("DC2_G764_VERIFY"); return on; }
void noteResult(bool hit, uint64_t pixels, bool replacement, uint64_t bad)
{
    static const bool stats = [] { const bool on = flag("DC2_G764_STATS") || flag("DC2_G764_VERIFY");
        if (on) std::atexit(report); return on; }();
    if (!stats) return;
    if (hit) { ++hits; texels += pixels; replaced += replacement; badPixels += bad; }
    else ++misses;
    const auto n = hits.load() + misses.load();
    if (bad || n <= 8 || n % 512 == 0) report();
}
void noteUpload(bool resident, uint64_t bytes)
{
    static const bool stats = flag("DC2_G764_STATS") || flag("DC2_G764_VERIFY");
    if (!stats) return;
    if (resident) { ++residentHits; avoidedBytes += bytes; }
    else { ++uploads; uploadBytes += bytes; }
}
}
