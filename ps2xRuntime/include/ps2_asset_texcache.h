#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Offline-compiled immutable asset textures. This interface deliberately accepts no
// filename or persistent VRAM address as identity. Every hit proves equality of ALL
// source bytes read by the supported decoder, including the palette. GPU-owned or
// framebuffer inputs must be rejected by the caller before lookup.
namespace dc2::assetcache
{
struct Descriptor
{
    uint32_t width = 0, height = 0, tbw = 0, psm = 0;
    bool operator==(const Descriptor&) const = default;
};

struct Image
{
    const uint32_t* pixels = nullptr;
    uint32_t width = 0, height = 0;
    bool replacement = false;
    // Reserved GPU namespace: bit 61 set, bits 62/63 clear. The process-lifetime
    // cache never reloads, so entry ordinals are collision-free resident identities.
    uint64_t residentKey = 0;
    const uint32_t* originalPixels = nullptr;
    explicit operator bool() const { return pixels != nullptr; }
};

class Cache
{
public:
    Cache();
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    // Transactional: malformed/unsupported files never publish a partial index.
    bool open(const std::string& path, std::string* error = nullptr);
    Image find(const Descriptor&, const uint8_t* vram, size_t vramSize,
               uint32_t textureBase, uint32_t paletteBase, bool replacements) const;
    size_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Process lifetime, read-only mapping: pointers remain valid across asynchronous
// GS batch submission. No eviction, runtime writes, reload, or disk access on hits.
const Cache* runtimeCache();
bool replacementsEnabled();
bool verifyEnabled();
void noteResult(bool hit, uint64_t pixels, bool replacement, uint64_t bad = 0);
void noteUpload(bool resident, uint64_t bytes);
}
