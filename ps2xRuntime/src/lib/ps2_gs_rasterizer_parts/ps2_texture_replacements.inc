// ps2_texture_replacements.inc — HD Texture Replacement & Dumping system for Dark Cloud 2
// Implements 100% PCSX2-compatible texture replacement format (<TEX0Hash>-<CLUTHash>-<TEX0_bits>.png).
// Isolated .inc file included directly in ps2_gs_rasterizer.cpp / runtime.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <algorithm>

#include "ps2_runtime_parts/dc2_logger.inc"

#define XXH_INLINE_ALL 1
#include "xxhash.h"

#ifndef STB_IMAGE_STATIC
#define STB_IMAGE_STATIC
#endif
#define STB_IMAGE_IMPLEMENTATION
#ifdef STBI_NO_STDIO
#undef STBI_NO_STDIO
#endif
#define STBI_ONLY_PNG
#include "stb_image.h"

#ifndef STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_STATIC
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace dc2_tex
{

// G620: control arm for the dead-probe early-out in `tryReplaceTexture` (see the comment there).
// Namespace scope so the read is a plain load, not an MSVC thread-safe-static guard (G619).
static const bool g_g620LiveTexRepl = (std::getenv("DC2_G620_LIVE_TEXREPL") != nullptr);

// PCSX2-compatible Texture Identifier Key (32 bytes)
struct TextureKey
{
    uint64_t tex0Hash = 0;
    uint64_t clutHash = 0;
    uint32_t bits = 0;

    bool operator==(const TextureKey &o) const
    {
        return tex0Hash == o.tex0Hash && clutHash == o.clutHash && bits == o.bits;
    }
};

struct TextureKeyHash
{
    size_t operator()(const TextureKey &k) const
    {
        uint64_t h = k.tex0Hash;
        h ^= k.clutHash + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<uint64_t>(k.bits) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

struct HashPair
{
    uint64_t tex0Hash = 0;
    uint64_t clutHash = 0;

    bool operator==(const HashPair &o) const
    {
        return tex0Hash == o.tex0Hash && clutHash == o.clutHash;
    }
};

struct HashPairHash
{
    size_t operator()(const HashPair &k) const
    {
        uint64_t h = k.tex0Hash;
        h ^= k.clutHash + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

struct DecodedImage
{
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // RGBA8
};

struct ReplacementStats
{
    std::atomic<uint64_t> texturesEncountered{0};
    std::atomic<uint64_t> staticCandidates{0};
    std::atomic<uint64_t> replacementHits{0};
    std::atomic<uint64_t> replacementMisses{0};
    std::atomic<uint64_t> dynamicRejected{0};
    std::atomic<uint64_t> loadFailures{0};
    std::atomic<uint64_t> texturesDumped{0};
};

class TextureReplacementManager
{
public:
    static TextureReplacementManager &instance()
    {
        static TextureReplacementManager s_instance;
        return s_instance;
    }

    void init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return;

        m_enabled = checkEnabled();
        m_dumpEnabled = checkDumpEnabled();
        m_hdDirectory = resolveDirectory("Mods/HD Texture");
        m_dumpDirectory = resolveDirectory("Mods/Texture Dump");

        if (m_enabled)
        {
            scanDirectory();
        }

        m_initialized = true;
    }

    void reload()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_replacementFiles.clear();
        m_replacementFilesNoTa.clear();
        m_replacementFilesNoTcc.clear();
        m_replacementByHashPair.clear();
        m_decodedCache.clear();
        m_negativeCache.clear();
        m_initialized = false;
        init();
        LOG_INFO(TextureReplacement, "Reloaded HD texture replacements (Indexed: %zu)", m_replacementFiles.size());
    }

    bool isEnabled() const { return m_enabled; }
    bool isDumpEnabled() const { return m_dumpEnabled; }
    const ReplacementStats &stats() const { return m_stats; }

    // Computes the standard PCSX2 TextureKey and formatted ID string for a GS texture
    static bool computeKey(uint8_t *vram, size_t vramSize,
                           const GSContext &ctx,
                           const GSTexaReg &texa,
                           const GSTexClutReg &texclut,
                           TextureKey &outKey, std::string &outIdStr)
    {
        if (!vram || vramSize == 0)
            return false;

        const uint32_t tw = ctx.tex0.tw;
        const uint32_t th = ctx.tex0.th;
        const uint32_t psm = ctx.tex0.psm;
        const uint32_t tbp0 = ctx.tex0.tbp0;
        const uint32_t tbw = ctx.tex0.tbw;

        const uint32_t texW = 1u << (tw & 0x0Fu);
        const uint32_t texH = 1u << (th & 0x0Fu);
        if (texW == 0 || texH == 0)
            return false;

        // 1. Compute TEX0_bits:
        // bits 0..5:   PSM (6 bits)
        // bits 6..9:   TW (4 bits)
        // bits 10..13: TH (4 bits)
        // bit 14:      unused / TCC (0)
        // bits 15..22: TA0 (8 bits)
        // bit 23:      AEM (1 bit)
        // bits 24..31: TA1 (8 bits)
        const uint32_t psmMasked = psm & 0x3Fu;
        const bool isPaletted = (psmMasked == 0x13u || psmMasked == 0x14u || psmMasked == 0x1Bu ||
                                 psmMasked == 0x24u || psmMasked == 0x2Cu);

        uint32_t bits = (psmMasked) |
                        ((tw & 0x0Fu) << 6) |
                        ((th & 0x0Fu) << 10) |
                        ((static_cast<uint32_t>(texa.ta0) & 0xFFu) << 15) |
                        ((texa.aem ? 1u : 0u) << 23) |
                        ((static_cast<uint32_t>(texa.ta1) & 0xFFu) << 24);

        // 2. Compute CLUTHash (if paletted)
        uint64_t clutHash = 0;
        if (isPaletted)
        {
            uint32_t clutTable[256];
            const size_t palCount = (psmMasked == 0x14u || psmMasked == 0x24u || psmMasked == 0x2Cu) ? 16u : 256u;
            for (size_t k = 0; k < palCount; ++k)
            {
                clutTable[k] = g178LookupClutFixed(vram, texa, texclut, static_cast<uint8_t>(k),
                                                   ctx.tex0.cbp, ctx.tex0.cpsm, ctx.tex0.csm,
                                                   ctx.tex0.csa, ctx.tex0.psm);
            }
            clutHash = XXH3_64bits(clutTable, palCount * sizeof(uint32_t));
        }

        // 3. Compute TEX0Hash over GS VRAM blocks
        uint32_t blockPixelsX = 8, blockPixelsY = 8;
        uint32_t pageBlocksX = 8, pageBlocksY = 4;

        if (psmMasked == 0x13u || psmMasked == 0x1Bu) // PSMT8 / PSMT8H
        {
            blockPixelsX = 16; blockPixelsY = 16;
            pageBlocksX = 8;   pageBlocksY = 4;
        }
        else if (psmMasked == 0x14u || psmMasked == 0x24u || psmMasked == 0x2Cu) // PSMT4 / PSMT4HL / PSMT4HH
        {
            blockPixelsX = 32; blockPixelsY = 16;
            pageBlocksX = 4;   pageBlocksY = 8;
        }
        else if (psmMasked == 0x02u || psmMasked == 0x0Au || psmMasked == 0x32u || psmMasked == 0x3Au) // 16-bit
        {
            blockPixelsX = 16; blockPixelsY = 8;
            pageBlocksX = 4;   pageBlocksY = 8;
        }
        else // 32-bit & 24-bit
        {
            blockPixelsX = 8;  blockPixelsY = 8;
            pageBlocksX = 8;   pageBlocksY = 4;
        }

        static constexpr uint8_t BlockTableC32[4][8] = {
            { 0,  1,  4,  5, 16, 17, 20, 21 },
            { 2,  3,  6,  7, 18, 19, 22, 23 },
            { 8,  9, 12, 13, 24, 25, 28, 29 },
            { 10, 11, 14, 15, 26, 27, 30, 31 }
        };

        static constexpr uint8_t BlockTableC16[8][4] = {
            {  0,  2,  8, 10 },
            {  1,  3,  9, 11 },
            {  4,  6, 12, 14 },
            {  5,  7, 13, 15 },
            { 16, 18, 24, 26 },
            { 17, 19, 25, 27 },
            { 20, 22, 28, 30 },
            { 21, 23, 29, 31 }
        };

        const uint32_t bw = std::max(1u, tbw);
        uint32_t bwPages = 1u;
        if (psmMasked == 0x13u || psmMasked == 0x14u || psmMasked == 0x1Bu ||
            psmMasked == 0x24u || psmMasked == 0x2Cu)
        {
            bwPages = std::max(1u, (bw * 64u) / 128u);
        }
        else if (psmMasked == 0x02u || psmMasked == 0x0Au || psmMasked == 0x32u || psmMasked == 0x3Au)
        {
            bwPages = std::max(1u, (bw * 64u) / 64u);
        }
        else
        {
            bwPages = bw;
        }

        const uint32_t numBlocksX = (texW + blockPixelsX - 1) / blockPixelsX;
        const uint32_t numBlocksY = (texH + blockPixelsY - 1) / blockPixelsY;
        const uint32_t basePage = (tbp0 >> 5);

        XXH3_state_t st;
        XXH3_64bits_reset(&st);

        for (uint32_t by = 0; by < numBlocksY; ++by)
        {
            const uint32_t py = by / pageBlocksY;
            const uint32_t blockY = by % pageBlocksY;

            for (uint32_t bx = 0; bx < numBlocksX; ++bx)
            {
                const uint32_t px = bx / pageBlocksX;
                const uint32_t blockX = bx % pageBlocksX;

                const uint32_t pageIndex = basePage + py * bwPages + px;
                const uint32_t blockInPage = (pageBlocksX == 8)
                    ? BlockTableC32[blockY][blockX]
                    : BlockTableC16[blockY][blockX];

                const uint32_t blockNum = ((pageIndex << 5) | blockInPage) & 0x3FFFu; // Wrap to 16,384 blocks
                const size_t byteOffset = static_cast<size_t>(blockNum) * 256u;

                if (byteOffset + 256u <= vramSize)
                {
                    XXH3_64bits_update(&st, vram + byteOffset, 256u);
                }
            }
        }

        const uint64_t tex0Hash = XXH3_64bits_digest(&st);

        outKey.tex0Hash = tex0Hash;
        outKey.clutHash = clutHash;
        outKey.bits = bits;

        char buf[128];
        if (isPaletted)
        {
            std::snprintf(buf, sizeof(buf), "%llx-%llx-%08x",
                          static_cast<unsigned long long>(tex0Hash),
                          static_cast<unsigned long long>(clutHash),
                          bits);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%llx-%08x",
                          static_cast<unsigned long long>(tex0Hash),
                          bits);
        }
        outIdStr = buf;
        return true;
    }

    // Attempt to replace a texture before uploading to the host GPU.
    // Returns true if a replacement was applied, false if original should be used.
    bool tryReplaceTexture(uint8_t *vram, size_t vramSize,
                           const GSContext &ctx,
                           const GSTexaReg &texa,
                           const GSTexClutReg &texclut,
                           bool isFboSrc, bool isViewKey,
                           int &inOutW, int &inOutH, std::vector<uint32_t> &inOutPx)
    {
        m_stats.texturesEncountered.fetch_add(1, std::memory_order_relaxed);

        // G620: with BOTH replacement and dumping off, everything below this point provably ends at
        // the `if (!m_enabled) return false;` a hundred lines down — but not before running
        // `isEligible` and then a full `computeKey`, which hashes the source VRAM pages AND the CLUT
        // and builds a `std::string`, once per whole-texture decode. `[G620:decstage]` priced that
        // dead probe at **0.52 ms/f on `dungeon1`, 24.8% of the entire decode bracket**. The only
        // effects skipped are the two diagnostic counters below (`texturesEncountered`, the one that
        // ranks this path, is kept above); `inOutW`/`inOutH`/`inOutPx` are untouched on this arm in
        // the original body as well, so the early return is exact. Nothing here is env-gated by a
        // new flag: it is the manager's OWN state, resolved once in `init()`.
        // `DC2_G620_LIVE_TEXREPL=1` is the CONTROL ARM: it restores the dead probe so the phase's
        // paired gate can run both arms out of one binary. Namespace scope (G619) — a function-local
        // `static const bool` is a per-access thread-safe-static guard on MSVC.
        if (!m_enabled && !m_dumpEnabled && !g_g620LiveTexRepl)
            return false;

        const uint32_t fbp = ctx.frame.fbp;
        const uint32_t tbp0 = ctx.tex0.tbp0;
        const uint32_t tbw = ctx.tex0.tbw;
        const uint32_t psm = ctx.tex0.psm;

        // Safety classification: Exclude dynamic / RTT / framebuffer surfaces
        if (!isEligible(fbp, tbp0, psm, inOutW, inOutH, isFboSrc, isViewKey))
        {
            m_stats.dynamicRejected.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        m_stats.staticCandidates.fetch_add(1, std::memory_order_relaxed);

        TextureKey key;
        std::string idStr;
        if (!computeKey(vram, vramSize, ctx, texa, texclut, key, idStr))
        {
            return false;
        }

        // Texture dumping support (if enabled)
        if (m_dumpEnabled && !inOutPx.empty())
        {
            dumpTexture(key, idStr, inOutW, inOutH, psm, tbp0, tbw, inOutPx.data());
        }

        if (!m_enabled)
        {
            return false;
        }

        // Check negative cache first (fast skip)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_negativeCache.find(key) != m_negativeCache.end())
            {
                m_stats.replacementMisses.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        // Check decoded in-memory cache
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto itDec = m_decodedCache.find(key);
            if (itDec != m_decodedCache.end())
            {
                const auto &img = itDec->second;
                inOutW = img->width;
                inOutH = img->height;
                inOutPx = img->pixels;
                m_stats.replacementHits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }

        // Lookup replacement filepath in index using PCSX2-compatible multi-tier fallback
        std::string filePath;
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Tier 1: Exact match (tex0Hash, clutHash, bits)
            auto it = m_replacementFiles.find(key);
            if (it != m_replacementFiles.end())
            {
                filePath = it->second;
            }

            // Tier 2: Canonical TCC=1 without TA (tex0Hash, clutHash, (bits & 0x3FFF) | 0x4000)
            if (filePath.empty())
            {
                TextureKey tccKey = key;
                tccKey.bits = (key.bits & 0x3FFFu) | 0x4000u;
                auto itTcc = m_replacementFiles.find(tccKey);
                if (itTcc != m_replacementFiles.end())
                {
                    filePath = itTcc->second;
                }
            }

            // Tier 3: Without TA (tex0Hash, clutHash, bits & 0x7FFF)
            if (filePath.empty())
            {
                TextureKey noTaKey = key;
                noTaKey.bits &= 0x7FFFu;
                auto itNoTa = m_replacementFilesNoTa.find(noTaKey);
                if (itNoTa != m_replacementFilesNoTa.end())
                {
                    filePath = itNoTa->second;
                }
            }

            // Tier 4: Without TCC & without TA (tex0Hash, clutHash, bits & 0x3FFF)
            if (filePath.empty())
            {
                TextureKey noTccKey = key;
                noTccKey.bits &= 0x3FFFu;
                auto itNoTcc = m_replacementFilesNoTcc.find(noTccKey);
                if (itNoTcc != m_replacementFilesNoTcc.end())
                {
                    filePath = itNoTcc->second;
                }
            }

            // Tier 5: Hash pair only (tex0Hash, clutHash)
            if (filePath.empty())
            {
                HashPair hp{key.tex0Hash, key.clutHash};
                auto itHp = m_replacementByHashPair.find(hp);
                if (itHp != m_replacementByHashPair.end())
                {
                    filePath = itHp->second;
                }
            }

            if (filePath.empty())
            {
                m_negativeCache.insert(key);
                m_stats.replacementMisses.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        // Load & decode PNG replacement on demand
        auto decoded = loadPng(filePath);
        if (!decoded)
        {
            LOG_WARN_ONCE(TextureReplacement, "Failed replacement: %s Reason: PNG decode failed. Using original texture.", idStr.c_str());
            m_stats.loadFailures.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_negativeCache.insert(key);
            return false;
        }

        LOG_ONCE_KEY(key.tex0Hash ^ key.clutHash, TextureReplacement,
                     "REPLACED id=%s original=%dx%d replacement=%dx%d (%s)",
                     idStr.c_str(), inOutW, inOutH, decoded->width, decoded->height,
                     std::filesystem::path(filePath).filename().string().c_str());

        inOutW = decoded->width;
        inOutH = decoded->height;
        inOutPx = decoded->pixels;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_decodedCache[key] = decoded;
        }

        m_stats.replacementHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    TextureReplacementManager() = default;

    static bool checkEnabled()
    {
        const char *env = std::getenv("DC2_TEXTURE_REPLACEMENTS");
        if (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 || std::strcmp(env, "TRUE") == 0))
            return true;
        const char *mods = std::getenv("DC2_MODS_ENABLED");
        if (mods && std::strcmp(mods, "1") == 0)
        {
            // Also enable if Mods enabled and HD Texture folder exists
            std::string dir = resolveDirectory("Mods/HD Texture");
            return std::filesystem::exists(dir);
        }
        return false;
    }

    static bool checkDumpEnabled()
    {
        const char *env = std::getenv("DC2_TEXTURE_DUMP");
        return env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 || std::strcmp(env, "TRUE") == 0);
    }

    static std::string resolveDirectory(const std::string &relPath)
    {
        if (const char *modsDir = std::getenv("DC2_MODS_DIR"))
        {
            std::filesystem::path p = std::filesystem::path(modsDir) / (relPath == "Mods/HD Texture" ? "HD Texture" : (relPath == "Mods/Texture Dump" ? "Texture Dump" : relPath));
            if (std::filesystem::exists(p))
                return p.string();
        }

        // Check relative to current working directory
        if (std::filesystem::exists(relPath))
            return std::filesystem::absolute(relPath).string();

        // Check launcher publish path
        const std::string launcherPath = "D:/ps2r/dc2_Launcher/publish/win-x64/" + relPath;
        if (std::filesystem::exists(launcherPath))
            return launcherPath;

        return relPath;
    }

    void scanDirectory()
    {
        if (!std::filesystem::exists(m_hdDirectory))
        {
            LOG_WARN(TextureReplacement, "HD Texture directory not found: %s", m_hdDirectory.c_str());
            return;
        }

        LOG_INFO(TextureReplacement, "Scanning %s for HD replacement textures...", m_hdDirectory.c_str());

        size_t totalFound = 0;
        size_t validIndexed = 0;
        size_t duplicates = 0;

        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(m_hdDirectory))
            {
                if (!entry.is_regular_file())
                    continue;

                const auto &path = entry.path();
                if (path.extension() != ".png" && path.extension() != ".PNG")
                    continue;

                ++totalFound;
                const std::string filename = path.filename().string();

                TextureKey key;
                if (parseFilename(filename, key))
                {
                    if (!m_replacementFiles.emplace(key, path.string()).second)
                    {
                        ++duplicates;
                    }
                    else
                    {
                        ++validIndexed;

                        TextureKey noTa = key;
                        noTa.bits &= 0x7FFFu;
                        m_replacementFilesNoTa.emplace(noTa, path.string());

                        TextureKey noTcc = key;
                        noTcc.bits &= 0x3FFFu;
                        m_replacementFilesNoTcc.emplace(noTcc, path.string());

                        HashPair hp{key.tex0Hash, key.clutHash};
                        m_replacementByHashPair.emplace(hp, path.string());
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR(TextureReplacement, "Error while scanning HD texture directory: %s", e.what());
        }

        LOG_INFO(TextureReplacement, "Directory: %s", m_hdDirectory.c_str());
        LOG_INFO(TextureReplacement, "Replacement PNGs indexed: %zu (Total files: %zu, duplicates: %zu)",
                 validIndexed, totalFound, duplicates);
    }

    static bool parseFilename(const std::string &filename, TextureKey &outKey)
    {
        // 3-part: "%llx-%llx-%x"
        uint64_t h0 = 0, h1 = 0;
        uint32_t b = 0;
        char dot = 0;

        if (std::sscanf(filename.c_str(), "%llx-%llx-%x%c", &h0, &h1, &b, &dot) == 4 && dot == '.')
        {
            outKey.tex0Hash = h0;
            outKey.clutHash = h1;
            outKey.bits = b;
            return true;
        }

        // 2-part: "%llx-%x"
        if (std::sscanf(filename.c_str(), "%llx-%x%c", &h0, &b, &dot) == 3 && dot == '.')
        {
            outKey.tex0Hash = h0;
            outKey.clutHash = 0;
            outKey.bits = b;
            return true;
        }

        return false;
    }

    std::shared_ptr<DecodedImage> loadPng(const std::string &path)
    {
        int w = 0, h = 0, channels = 0;
        unsigned char *data = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!data)
            return nullptr;

        if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        {
            stbi_image_free(data);
            return nullptr;
        }

        auto img = std::make_shared<DecodedImage>();
        img->width = w;
        img->height = h;
        img->pixels.resize(static_cast<size_t>(w) * h);

        std::memcpy(img->pixels.data(), data, static_cast<size_t>(w) * h * 4u);
        stbi_image_free(data);

        return img;
    }

    static bool isEligible(uint32_t fbp, uint32_t tbp0, uint32_t psm, int texW, int texH,
                           bool isFboSrc, bool isViewKey)
    {
        if (isFboSrc || isViewKey)
            return false;

        if (texW <= 0 || texH <= 0)
            return false;

        // Framebuffer addresses in Dark Cloud 2:
        // 0x000: Front display buffer
        // 0x068: Back display buffer
        // 0x139: Primary 3D world scene framebuffer
        // 0x13b, 0x13d, 0x141, 0x146, 0x15d, 0x168, 0x240: Intermediate RTT / shadow / post-process targets
        // 0x2720: Dynamic map / effect atlas
        if (tbp0 == 0x000u || tbp0 == 0x068u || tbp0 == 0x139u || tbp0 == 0x13bu ||
            tbp0 == 0x13du || tbp0 == 0x141u || tbp0 == 0x146u || tbp0 == 0x15du ||
            tbp0 == 0x168u || tbp0 == 0x240u || tbp0 == 0x2720u)
        {
            return false;
        }

        return true;
    }

    void dumpTexture(const TextureKey &key, const std::string &idStr,
                     int origW, int origH, uint32_t psm, uint32_t tbp0, uint32_t tbw,
                     const uint32_t *rgbaPixels)
    {
        // Skip dumping extremely small textures (often single pixel colors, 
        // noise blocks, or garbage data) that clutter the dump folder.
        if (origW <= 16 && origH <= 16)
            return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_dumpedKeys.insert(key).second)
                return;
        }

        try
        {
            if (!std::filesystem::exists(m_dumpDirectory))
            {
                std::filesystem::create_directories(m_dumpDirectory);
            }

            const std::string pngPath = (std::filesystem::path(m_dumpDirectory) / (idStr + ".png")).string();

            if (stbi_write_png(pngPath.c_str(), origW, origH, 4, rgbaPixels, origW * 4))
            {
                m_stats.texturesDumped.fetch_add(1, std::memory_order_relaxed);
                LOG_ONCE_KEY(key.tex0Hash, TextureReplacement, "Dumped texture: %s (%dx%d)", idStr.c_str(), origW, origH);
            }
        }
        catch (...)
        {
            // Ignore dump disk errors cleanly
        }
    }

    bool m_initialized = false;
    bool m_enabled = false;
    bool m_dumpEnabled = false;
    std::string m_hdDirectory;
    std::string m_dumpDirectory;

    std::unordered_map<TextureKey, std::string, TextureKeyHash> m_replacementFiles;
    std::unordered_map<TextureKey, std::string, TextureKeyHash> m_replacementFilesNoTa;
    std::unordered_map<TextureKey, std::string, TextureKeyHash> m_replacementFilesNoTcc;
    std::unordered_map<HashPair, std::string, HashPairHash> m_replacementByHashPair;

    std::unordered_map<TextureKey, std::shared_ptr<DecodedImage>, TextureKeyHash> m_decodedCache;
    std::unordered_set<TextureKey, TextureKeyHash> m_negativeCache;
    std::unordered_set<TextureKey, TextureKeyHash> m_dumpedKeys;

    ReplacementStats m_stats;
    std::mutex m_mutex;
};

} // namespace dc2_tex
