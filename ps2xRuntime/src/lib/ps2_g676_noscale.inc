// ============================================================================================
// G676 — THE 2D "DO NOT SCALE" LIST.
// ============================================================================================
//
// `DC2_2D_SCALE=uniform` (the default) scales screen-space sprites about the logical screen centre
// so the HUD keeps its 4:3 proportions under a widened field of view. That is right for HUD
// furniture and WRONG for anything world-aligned or screen-covering — G675 §6.9 measured the
// dungeon caustics (a 5x5 grid of 128x128 PSMT8 tiles) turning into rainbow blocks, and no field in
// `G178Draw` distinguishes such a tile from a HUD widget.
//
// So the discrimination is made by IDENTITY instead of by heuristic: a curated list of images that
// must never be scaled. `tools/g676_extract_images.py` extracts all 762 2D assets from the retail
// ISO to PNG with a manifest, so the list is filled by LOOKING at the pictures.
//
// ⭐ THE KEY IS A CONTENT HASH OF THE DECODED RGB, NOT AN ADDRESS.
//   * not TBP/TBW — those are VRAM addresses the game reuses between scenes;
//   * not the asset path — the runtime never sees it (the GS only ever sees texels);
//   * RGB and not RGBA, because the ISO tool and the runtime disagree about the ALPHA convention
//     (PS2 alpha is 0..128, a PNG's is 0..255) and that difference must not change the key.
//   * seeded with the texture's own w/h so two different-sized images cannot collide.
//
// FNV-1a 64 rather than a cryptographic hash: this runs once per unique texture DECODE, on the GS
// worker, and only when the list is non-empty — collision risk over a few hundred curated images is
// negligible, and the cost has to stay near zero.
//
// FILE FORMAT — `Mods/2d_noscale.txt`, one entry per line:
//     # comments and blank lines ignored
//     a1b2c3d4e5f60718              <- the `key` column from Mods/2D Images/manifest.csv
//     a1b2c3d4e5f60718  menu/foo    <- anything after the hash is a free-text label
//
// ⛔ FMV IS NOT ON THIS LIST AND MUST NOT BE. A movie is not a texture the GS uploads once; it is a
// stream of frames, so it has no stable content hash by construction. It is excluded structurally
// instead — `dc2FmvActiveTick()` in ps2_widescreen.inc presents the whole frame at 4:3 while a
// movie is on screen, which also disarms sprite scaling for it. See G675 §6.10.

#ifndef PS2X_G676_NOSCALE_INC
#define PS2X_G676_NOSCALE_INC

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dc2_noscale
{

inline uint64_t hashTexels(const uint32_t *rgba, int w, int h)
{
    // FNV-1a 64 over w, h, then the RGB byte of every texel in row order.
    uint64_t v = 1469598103934665603ull;
    const auto mix = [&v](uint8_t b) {
        v ^= static_cast<uint64_t>(b);
        v *= 1099511628211ull;
    };
    const uint32_t dims[2] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    for (int d = 0; d < 2; ++d)
        for (int k = 0; k < 4; ++k)
            mix(static_cast<uint8_t>((dims[d] >> (k * 8)) & 0xFFu));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i)
    {
        const uint32_t p = rgba[i];
        mix(static_cast<uint8_t>(p & 0xFFu));         // R
        mix(static_cast<uint8_t>((p >> 8) & 0xFFu));  // G
        mix(static_cast<uint8_t>((p >> 16) & 0xFFu)); // B
    }
    return v;
}

// ⛔⛔ MEASURED: THE ISO ASSETS ARE NOT THE RUNTIME'S TEXTURES, so a list keyed off the archive
// extraction cannot drive this. On the Items route the runtime decoded 2,141 distinct textures, of
// which **1,971 were 256x256** — the game composes its own VRAM atlas pages at upload time, mixing
// several archive images (and its own CLUT/TEXA state) into one page. Against the 671 distinct
// archive pictures the intersection was **2**: `img/water_ref` and `meswin/usa/gaiji`, the only two
// uploaded whole and used as-is. The ISO extraction is therefore for BROWSING, and the list has to
// be keyed on what the GS actually samples — which is what `DC2_2D_TEXDUMP=1` writes out.
//
// Hence two lists, and the ALLOW-list is the useful one:
//   Mods/2d_noscale.txt  — deny-list: these keys never scale. Everything else follows the mode.
//   Mods/2d_scale.txt    — allow-list: with `DC2_2D_SCALE=list`, ONLY these keys scale.
// `list` mode is the safe way to curate: nothing scales until it is approved, so a scene that has
// never been looked at cannot be broken by a heuristic.
struct State
{
    bool loaded = false;
    bool active = false;                     // a non-empty deny-list was found
    bool allowLoaded = false;
    std::unordered_set<uint64_t> keys;       // hashes that must not be scaled
    std::unordered_set<uint64_t> allow;      // hashes that MAY scale (list mode)
    std::mutex mtx;
    std::unordered_map<uint64_t, bool> byTexKey;      // GS texKey -> on deny-list
    std::unordered_map<uint64_t, bool> allowByTexKey; // GS texKey -> on allow-list
};

inline State &state()
{
    static State s;
    return s;
}

inline void load()
{
    State &s = state();
    if (s.loaded)
        return;
    s.loaded = true;

    const char *envPath = std::getenv("DC2_2D_NOSCALE_LIST");
    const char *path = (envPath && envPath[0] != '\0') ? envPath : "Mods/2d_noscale.txt";
    std::FILE *fh = std::fopen(path, "rb");
    if (fh == nullptr)
        return;

    char line[512];
    while (std::fgets(line, sizeof(line), fh) != nullptr)
    {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        char *end = nullptr;
        const uint64_t key = std::strtoull(p, &end, 16);
        if (end != p && key != 0ull)
            s.keys.insert(key);
    }
    std::fclose(fh);
    s.active = !s.keys.empty();
    std::fprintf(stderr, "[G676:noscale] %s: %zu entries%s\n", path, s.keys.size(),
                 s.active ? "" : " (inactive)");
}

// True when a list exists — every hook below is a single bool load when it does not.
inline bool active()
{
    State &s = state();
    if (!s.loaded)
        load();
    return s.active;
}

// Called once per unique texture DECODE with the decoded RGBA. Records whether this GS texKey
// resolves to a listed image, so the per-draw test is a hash-map probe and never a re-hash.
inline std::unordered_set<uint64_t> &allowSet()
{
    State &s = state();
    if (!s.allowLoaded)
    {
        s.allowLoaded = true;
        const char *envPath = std::getenv("DC2_2D_SCALE_LIST");
        const char *path = (envPath && envPath[0] != '\0') ? envPath : "Mods/2d_scale.txt";
        if (std::FILE *fh = std::fopen(path, "rb"))
        {
            char line[512];
            while (std::fgets(line, sizeof(line), fh) != nullptr)
            {
                char *p = line;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
                    continue;
                char *end = nullptr;
                const uint64_t key = std::strtoull(p, &end, 16);
                if (end != p && key != 0ull)
                    s.allow.insert(key);
            }
            std::fclose(fh);
            std::fprintf(stderr, "[G676:scalelist] %s: %zu entries\n", path, s.allow.size());
        }
    }
    return s.allow;
}

// Vertex preparation may overlap a decode only when that decode cannot change this flush's sprite
// classification. Loading both lists here also keeps their file I/O on the GS owner, never on the
// asynchronous preparation lane.
inline bool needsDecodedClassification()
{
    return active() || !allowSet().empty();
}

inline void noteDecodedTexture(uint64_t texKey, const uint32_t *rgba, int w, int h)
{
    if (texKey == 0ull || rgba == nullptr || w <= 0 || h <= 0)
        return;
    State &s = state();
    const bool needDeny = active();
    const bool needAllow = !allowSet().empty();
    if (!needDeny && !needAllow)
        return;
    const uint64_t hash = hashTexels(rgba, w, h);
    const bool listed = needDeny && (s.keys.find(hash) != s.keys.end());
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.byTexKey[texKey] = listed;
        if (needAllow)
            s.allowByTexKey[texKey] = (s.allow.find(hash) != s.allow.end());
    }
    if (listed)
    {
        static std::unordered_set<uint64_t> reported;
        static std::mutex rm;
        std::lock_guard<std::mutex> lk(rm);
        if (reported.insert(hash).second)
            std::fprintf(stderr, "[G676:noscale] matched %016llx (%dx%d) -> will not scale\n",
                         static_cast<unsigned long long>(hash), w, h);
    }
}

// Per-draw test. False (scale it) unless the texture was decoded and matched.
inline bool texKeyIsListed(uint64_t texKey)
{
    if (!active() || texKey == 0ull)
        return false;
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    const auto it = s.byTexKey.find(texKey);
    return it != s.byTexKey.end() && it->second;
}

// `DC2_2D_SCALE=list`: scale ONLY what the allow-list names.
inline bool allowListMode()
{
    static const bool on = []() {
        const char *v = std::getenv("DC2_2D_SCALE");
        return v != nullptr && (std::strcmp(v, "list") == 0 || std::strcmp(v, "LIST") == 0);
    }();
    return on;
}

inline bool texKeyIsAllowed(uint64_t texKey)
{
    if (texKey == 0ull || allowSet().empty())
        return false;
    State &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    const auto it = s.allowByTexKey.find(texKey);
    return it != s.allowByTexKey.end() && it->second;
}

// ===== DC2_2D_TEXDUMP=1 — write what the GS ACTUALLY samples, keyed identically to the lists ====
//
// Binary PPM (P6) rather than PNG: no encoder dependency, and the project already ships
// `tools/ppm2png.py`. `tools/g676_runtime_png.py` converts a whole dump directory and writes a
// manifest, so the workflow is: play -> convert -> browse -> paste keys.
inline bool texDumpOn()
{
    static const bool on = (std::getenv("DC2_2D_TEXDUMP") != nullptr);
    return on;
}

inline void dumpDecodedTexture(const uint32_t *rgba, int w, int h)
{
    if (!texDumpOn() || rgba == nullptr || w <= 0 || h <= 0)
        return;
    const uint64_t hash = hashTexels(rgba, w, h);
    static std::unordered_set<uint64_t> seen;
    static std::mutex m;
    {
        std::lock_guard<std::mutex> lk(m);
        if (seen.size() >= 8192u || !seen.insert(hash).second)
            return;
    }
    char path[256];
    std::snprintf(path, sizeof(path), "Mods/2D Runtime/%016llx_%dx%d.ppm",
                  static_cast<unsigned long long>(hash), w, h);
    if (std::FILE *fh = std::fopen(path, "wb"))
    {
        std::fprintf(fh, "P6\n%d %d\n255\n", w, h);
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::string rgb;
        rgb.resize(n * 3u);
        for (size_t i = 0; i < n; ++i)
        {
            rgb[i * 3 + 0] = static_cast<char>(rgba[i] & 0xFFu);
            rgb[i * 3 + 1] = static_cast<char>((rgba[i] >> 8) & 0xFFu);
            rgb[i * 3 + 2] = static_cast<char>((rgba[i] >> 16) & 0xFFu);
        }
        std::fwrite(rgb.data(), 1, rgb.size(), fh);
        std::fclose(fh);
    }
}

// Diagnostic: DC2_2D_NOSCALE_DUMP=1 prints the key of every decoded texture, so an image can be
// identified without going through the ISO extractor.
inline bool dumpKeys()
{
    static const bool on = (std::getenv("DC2_2D_NOSCALE_DUMP") != nullptr);
    return on;
}

inline void noteDecodedKeyDump(const uint32_t *rgba, int w, int h)
{
    if (!dumpKeys() || rgba == nullptr || w <= 0 || h <= 0)
        return;
    static std::unordered_set<uint64_t> seen;
    static std::mutex m;
    const uint64_t hash = hashTexels(rgba, w, h);
    std::lock_guard<std::mutex> lk(m);
    if (seen.size() < 4096u && seen.insert(hash).second)
        std::fprintf(stderr, "[G676:key] %016llx %dx%d\n",
                     static_cast<unsigned long long>(hash), w, h);
}

} // namespace dc2_noscale

#endif // PS2X_G676_NOSCALE_INC
