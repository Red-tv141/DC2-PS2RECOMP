// G399 — Dungeon 6 free-roam darkening: direct render-target surface probe.
//
// G398 pinned the break to the first free-roam frame (the frame DC2 switches on its
// fbp=0x068 / fbp=0x000 post-process + composite that reads fbp=0x139 back as
// tbp=0x2720 PSMCT32) and proved the GS command stream matches hardware down to vertex
// colours. The remaining unknown is whether the loss happens *into* 0x139 or *after* it,
// in the 0x139 -> 0x2720 readback/alias or the composite sample.
//
// This probe renders the raw VRAM contents of each surface in that chain directly as
// PSMCT32 and reports per-surface mean luminance, so the two cases can be told apart:
//   * 0x139 already dark  => loss is in the multi-target draw path INTO 0x139.
//   * 0x139 correct, 0x068/0x000 dark => loss is in the readback/alias or composite.
//
// Diagnostic only, default-off, no behaviour change.
//   DC2_G399_SURFDUMP=1        enable (also forces the raw-VRAM diag path, see
//                              g289RawVramDiagOn(), so VRAM is authoritative under G178).
//   DC2_G399_EVERY=<n>         report every n-th latch (default 60).
//   DC2_G399_PPM=<n>           also write PPMs on latches that are a multiple of n
//                              (default 0 = never; stats-only is cheap).
//   DC2_G399_FBPS=<hex,hex>    surface list (default "139,68,0").

static bool g399SurfDumpOn()
{
    static const bool s_on = (std::getenv("DC2_G399_SURFDUMP") != nullptr);
    return s_on;
}

static uint32_t g399EnvUInt(const char *name, uint32_t fallback)
{
    const char *e = std::getenv(name);
    if (!e || !*e)
        return fallback;
    return static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
}

// Parse DC2_G399_FBPS ("139,68,0" — always hex, no 0x prefix needed) once.
static const std::vector<uint32_t> &g399SurfaceList()
{
    static const std::vector<uint32_t> s_list = [] {
        std::vector<uint32_t> out;
        const char *e = std::getenv("DC2_G399_FBPS");
        if (e && *e)
        {
            const char *p = e;
            while (*p)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p)
                    break;
                out.push_back(static_cast<uint32_t>(v));
                p = end;
                while (*p == ',' || *p == ' ')
                    ++p;
            }
        }
        if (out.empty())
            out = {0x139u, 0x068u, 0x000u};
        return out;
    }();
    return s_list;
}

// Render one PSMCT32 surface out of raw VRAM and report its statistics.
// Width/height are the DC2 dungeon RTT footprint (512x416, fbw=8 pages).
static void g399ProbeSurfaces(const uint8_t *vram, uint32_t vramSize)
{
    if (!g399SurfDumpOn() || !vram || vramSize == 0u)
        return;

    static const uint32_t s_every = g399EnvUInt("DC2_G399_EVERY", 60u);
    static const uint32_t s_ppmEvery = g399EnvUInt("DC2_G399_PPM", 0u);
    if (s_every == 0u)
        return;

    static std::atomic<uint32_t> s_latch{0u};
    const uint32_t n = s_latch.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % s_every) != 0u)
        return;
    const bool writePpm = (s_ppmEvery != 0u) && ((n % s_ppmEvery) == 0u);

    constexpr uint32_t kW = 512u;
    constexpr uint32_t kH = 416u;
    constexpr uint32_t kFbwPages = 8u;

    for (uint32_t fbp : g399SurfaceList())
    {
        const uint32_t base = GSInternal::framePageBaseToBlock(fbp);

        double sumL = 0.0;
        double sumA = 0.0;
        uint64_t nonZero = 0ull;
        uint32_t maxL = 0u;
        std::vector<uint8_t> rgb;
        if (writePpm)
            rgb.resize(static_cast<size_t>(kW) * kH * 3u);

        for (uint32_t y = 0u; y < kH; ++y)
        {
            for (uint32_t x = 0u; x < kW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(base, kFbwPages, x, y);
                uint32_t px = 0u;
                if (off + 4u <= vramSize)
                    std::memcpy(&px, vram + off, 4u);
                const uint32_t r = px & 0xFFu;
                const uint32_t g = (px >> 8) & 0xFFu;
                const uint32_t b = (px >> 16) & 0xFFu;
                const uint32_t a = (px >> 24) & 0xFFu;
                // Same luminance definition the frame-census/brightness tooling uses.
                const uint32_t l = (r * 299u + g * 587u + b * 114u) / 1000u;
                sumL += static_cast<double>(l);
                sumA += static_cast<double>(a);
                if ((px & 0x00FFFFFFu) != 0u)
                    ++nonZero;
                if (l > maxL)
                    maxL = l;
                if (writePpm)
                {
                    const size_t o = (static_cast<size_t>(y) * kW + x) * 3u;
                    rgb[o + 0u] = static_cast<uint8_t>(r);
                    rgb[o + 1u] = static_cast<uint8_t>(g);
                    rgb[o + 2u] = static_cast<uint8_t>(b);
                }
            }
        }

        const double pixels = static_cast<double>(kW) * static_cast<double>(kH);
        std::fprintf(stderr,
                     "[G399:surf] n=%u fbp=0x%03x meanL=%.2f maxL=%u meanA=%.2f nz=%llu/%.0f\n",
                     n, fbp, sumL / pixels, maxL, sumA / pixels,
                     static_cast<unsigned long long>(nonZero), pixels);

        if (writePpm)
        {
            char path[160];
            std::snprintf(path, sizeof(path),
                          "D:/ps2r/dc2/captures/g399_surf_%03x_%06u.ppm", fbp, n);
            if (FILE *fp = std::fopen(path, "wb"))
            {
                std::fprintf(fp, "P6\n%u %u\n255\n", kW, kH);
                std::fwrite(rgb.data(), 1u, rgb.size(), fp);
                std::fclose(fp);
                std::fprintf(stderr, "[G399:surf] wrote %s\n", path);
            }
        }
    }
}
