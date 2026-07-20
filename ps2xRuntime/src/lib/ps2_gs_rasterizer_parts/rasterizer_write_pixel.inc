void GSRasterizer::writePixel(GS *gs, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const auto &ctx = gs->activeContext();
    // PHASE F30: writePixel entry counter, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint64_t> s_f30PxEntry{0};
        const uint64_t n = s_f30PxEntry.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 8ull || (n & 0xFFFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F30:px.enter] n=%llu x=%d y=%d fbp=0x%x fbw=%u psm=%u sciss=[%d,%d,%d,%d]\n",
                         static_cast<unsigned long long>(n), x, y,
                         ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm,
                         ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
    }
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 ||
        y < ctx.scissor.y0 || y > ctx.scissor.y1)
    {
        if (phaseDiagnosticsEnabled())
        {
        static std::atomic<uint64_t> s_f30Sciss{0};
        const uint64_t n = s_f30Sciss.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 4ull || (n & 0x3FFFull) == 0ull)
        {
            std::fprintf(stderr,
                         "[F30:px.scissCull] n=%llu x=%d y=%d sciss=[%d,%d,%d,%d]\n",
                             static_cast<unsigned long long>(n), x, y,
                             ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        }
        }
        return;
    }

    const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test, a);
    if (!alphaTest.writeFramebuffer)
    {
        if (phaseDiagnosticsEnabled())
        {
        static std::atomic<uint64_t> s_f30Alpha{0};
        const uint64_t n = s_f30Alpha.fetch_add(1, std::memory_order_relaxed) + 1ull;
        if (n <= 4ull || (n & 0x3FFFull) == 0ull)
        {
            std::fprintf(stderr, "[F30:px.alphaCull] n=%llu a=%u\n",
                         static_cast<unsigned long long>(n), a);
        }
        }
        return;
    }

    // PHASE F30: writePixel commit counter — track RGB-nonzero @ fbp=0x68 separately
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint64_t> s_f30PxCommit68{0};
        static std::atomic<uint64_t> s_f30PxRgbNz68{0};
        static std::atomic<uint64_t> s_f30PxRgbNz68Logged{0};
        if (ctx.frame.fbp == 0x68u)
        {
            const uint64_t k = s_f30PxCommit68.fetch_add(1, std::memory_order_relaxed) + 1ull;
            const bool rgbNz = (r | g | b) != 0u;
            if (rgbNz)
            {
                s_f30PxRgbNz68.fetch_add(1, std::memory_order_relaxed);
                const uint64_t lg = s_f30PxRgbNz68Logged.fetch_add(1, std::memory_order_relaxed) + 1ull;
                if (lg <= 8ull || (lg & 0x1FFFFull) == 0ull)
                {
                    std::fprintf(stderr,
                                 "[F30:px.rgbNz68] lg=%llu x=%d y=%d rgba=%02x%02x%02x%02x\n",
                                 static_cast<unsigned long long>(lg), x, y, r, g, b, a);
                }
            }
            if ((k & 0xFFFFFull) == 0ull)
            {
                std::fprintf(stderr,
                             "[F30:px.summary68] total=%llu rgbNz=%llu\n",
                             static_cast<unsigned long long>(k),
                             static_cast<unsigned long long>(s_f30PxRgbNz68.load()));
            }
        }
    }

    const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
    const uint32_t bytesPerPixel =
        (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S) ? 2u : 4u;

    uint32_t off = 0u;
    if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
    {
        off = GSPSMCT32::addrPSMCT32(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                     widthBlocks,
                                     static_cast<uint32_t>(x),
                                     static_cast<uint32_t>(y));
    }
    else
    {
        off = addrPSMCT16Family(GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                widthBlocks,
                                ctx.frame.psm,
                                static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y));
    }

    if (off + bytesPerPixel > gs->m_vramSize)
        return;

    // G219 (default-off, DC2_G219_SKYPX=1): watch writes landing in the qwords the [G219:dif]
    // capture-vs-replay diff showed being zeroed (0x278400..) — identifies the clobbering draw.
    // (The F51.2 watch below can't serve: renderQualityTraceEnabled() disables deferral itself.)
    {
        static const bool s_g219w2 = (std::getenv("DC2_G219_SKYPX") != nullptr);
        if (s_g219w2 && off >= 0x278400u && off < 0x278440u)
        {
            static std::atomic<uint32_t> s_g219wpn{0};
            const uint32_t n = s_g219wpn.fetch_add(1u, std::memory_order_relaxed);
            if (n < 400u || (n % 512u) == 0u)
                std::fprintf(stderr,
                    "[G219:wpx] n=%u off=0x%x xy=(%d,%d) fbp=0x%x fbw=%u fpsm=0x%x prim=%u tme=%u "
                    "tbp=0x%x rgba=%02x,%02x,%02x,%02x replay=%u\n",
                    n, off, x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                    static_cast<uint32_t>(ctx.frame.psm), static_cast<uint32_t>(gs->m_prim.type),
                    static_cast<uint32_t>(gs->m_prim.tme), ctx.tex0.tbp0, r, g, b, a,
                    t_g144InReplay ? 1u : 0u);
        }
    }

    // [F51.2] Watch writes that land in the manager texture page (0x272000..0x282000).
    // Identify which draw zeroes the uploaded T8 data (pageNz drops 62381 -> ~8192).
    if (renderQualityTraceEnabled() && off >= 0x272000u && off < 0x282000u)
    {
        static std::atomic<uint32_t> s_f512Watch{0};
        const uint32_t wn = s_f512Watch.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool rgbZero = (r | g | b) == 0u;
        if (wn <= 32u || (rgbZero && wn <= 2000u && (wn % 200u) == 0u))
        {
            std::fprintf(stderr,
                "[F51.2:texwrite] wn=%u off=0x%x x=%d y=%d fbp=0x%x fbw=%u fpsm=0x%x "
                "tme=%u abe=%u rgba=%02x,%02x,%02x,%02x tbp=0x%x tpsm=0x%x\n",
                wn, off, x, y, ctx.frame.fbp, static_cast<uint32_t>(ctx.frame.fbw),
                static_cast<uint32_t>(ctx.frame.psm),
                static_cast<uint32_t>(gs->m_prim.tme), static_cast<uint32_t>(gs->m_prim.abe),
                r, g, b, a, ctx.tex0.tbp0, static_cast<uint32_t>(ctx.tex0.psm));
        }
    }

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t pixelIndex = s_debugPixelCount.fetch_add(1, std::memory_order_relaxed);
        if (pixelIndex < 32u)
        {
            std::cout << "[gs:pixel] idx=" << pixelIndex
                      << " xy=(" << x << "," << y << ")"
                      << " rgba=(" << static_cast<uint32_t>(r) << ","
                      << static_cast<uint32_t>(g) << ","
                      << static_cast<uint32_t>(b) << ","
                      << static_cast<uint32_t>(a) << ")"
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " off=0x" << std::hex << off << std::dec
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if (ctx.frame.fbp == 150u &&
            s_debugFbp150PixelCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:fbp150-pixel]"
                      << " xy=(" << x << "," << y << ")"
                      << " rgba=(" << static_cast<uint32_t>(r) << ","
                      << static_cast<uint32_t>(g) << ","
                      << static_cast<uint32_t>(b) << ","
                      << static_cast<uint32_t>(a) << ")"
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " off=0x" << std::hex << off << std::dec << std::endl;
        }
    });

    const uint8_t srcR = r;
    const uint8_t srcG = g;
    const uint8_t srcB = b;
    const uint8_t srcA = a;

    if (gs->m_prim.abe)
    {
        uint32_t existing = 0u;
        if (bytesPerPixel == 2u)
        {
            uint16_t packed = 0u;
            std::memcpy(&packed, gs->m_vram + off, 2);
            existing = decodePSMCT16(packed);
        }
        else
        {
            std::memcpy(&existing, gs->m_vram + off, 4);
        }
        uint8_t dr = existing & 0xFF;
        uint8_t dg = (existing >> 8) & 0xFF;
        uint8_t db = (existing >> 16) & 0xFF;
        uint8_t da = (existing >> 24) & 0xFF;

        // G48: the costume model (fbp=0x139) renders into an RTT the runner CLEARS TO BLACK. PS2
        // alpha-over (Cv=(Cs-Cd)*A>>7+Cd) against a black dest = Cs*A/128, which DARKENS or hides
        // any sub-mesh whose source alpha < 128 when it is the FIRST fragment at a pixel. On real
        // HW the opaque base mesh covers a pixel before the alpha-blended detail/hat/face decals
        // land, so those blends meet real color, not black. The runner's head sub-meshes interleave
        // (skin / hair / hat-decal / face decals) so decals frequently hit the still-black RTT first
        // -> the hat (a decal page) vanished under the old "skip decal over black" heuristic, and
        // face decals over black showed as dark dots. Reproduce HW: when the costume RTT dest is
        // still black (uncovered), write the source color OPAQUELY (skip the blend) for ANY head
        // mesh; subsequent fragments then blend over real color. Replaces the G45 NoDot decal-skip.
        const bool g48OpaqueOverBlack =
            (ctx.frame.fbp == 0x139u) && ((existing & 0x00FFFFFFu) == 0u);
        if (g48OpaqueOverBlack)
        {
            r = srcR; g = srcG; b = srcB;
        }
        // PABE disables alpha blending when the source alpha MSB is clear.
        else if (!(gs->m_pabe && (a & 0x80u) == 0u))
        {
            uint64_t alphaReg = ctx.alpha;
            uint8_t asel = alphaReg & 3;
            uint8_t bsel = (alphaReg >> 2) & 3;
            uint8_t csel = (alphaReg >> 4) & 3;
            uint8_t dsel = (alphaReg >> 6) & 3;
            uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                       : fix;

            r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
            g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
            b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            if (g258TraceOn() && g_g255Verify.pending && g_g255Verify.fbp == 0x146u &&
                g_g255Verify.traceBatch <= 4u && x == 52 && y == 1)
            {
                const int nr = (pickRGB(asel, srcR, dr) - pickRGB(bsel, srcR, dr)) * cAlpha;
                const int ng = (pickRGB(asel, srcG, dg) - pickRGB(bsel, srcG, dg)) * cAlpha;
                const int nb = (pickRGB(asel, srcB, db) - pickRGB(bsel, srcB, db)) * cAlpha;
                std::fprintf(stderr,
                    "[G258:cpu.blend] batch=%llu entry=%zu dst=%02x%02x%02x%02x "
                    "src=%02x%02x%02x%02x c=%d num=(%d,%d,%d) out=%02x%02x%02x%02x\n",
                    static_cast<unsigned long long>(g_g255Verify.traceBatch), t_g258EntryIndex,
                    dr, dg, db, da, srcR, srcG, srcB, srcA, cAlpha, nr, ng, nb,
                    r, g, b, srcA);
            }
        }
        else
        {
            r = srcR;
            g = srcG;
            b = srcB;
        }
    }

    uint32_t mask = ctx.frame.fbmsk;
    if (!alphaTest.preserveDestinationAlpha &&
        (ctx.fba & 0x1ull) != 0ull &&
        ctx.frame.psm != GS_PSM_CT24)
    {
        a = static_cast<uint8_t>(a | 0x80u);
    }

    if (bytesPerPixel == 2u)
    {
        uint16_t pixel = encodePSMCT16(r, g, b, a);
        if ((mask & 0xFFFFu) != 0u)
        {
            uint16_t existing = 0u;
            std::memcpy(&existing, gs->m_vram + off, 2);
            pixel = static_cast<uint16_t>((pixel & ~mask) | (existing & mask));
        }
        if (g195ForegroundTraceTarget(ctx, x, y))
        {
            uint16_t before = 0u;
            std::memcpy(&before, gs->m_vram + off, 2);
            g195LogForegroundPixel(ctx, x, y, off, bytesPerPixel,
                                   static_cast<uint32_t>(gs->m_prim.type),
                                   gs->m_prim.tme, gs->m_prim.abe,
                                   gs->m_prim.fst, gs->m_prim.ctxt, gs->m_pabe,
                                   decodePSMCT16(before), srcR, srcG, srcB, srcA,
                                   decodePSMCT16(pixel));
        }
        f50_12_log_framebuffer_commit(ctx, x, y, off, bytesPerPixel, decodePSMCT16(pixel),
                                      gs->m_prim.type, gs->m_prim.tme, gs->m_prim.abe,
                                      gs->m_prim.fst, gs->m_prim.ctxt);
        std::memcpy(gs->m_vram + off, &pixel, 2);
        return;
    }

    uint32_t pixel = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);

    if (mask != 0)
    {
        uint32_t existing;
        std::memcpy(&existing, gs->m_vram + off, 4);
        pixel = (pixel & ~mask) | (existing & mask);
    }

    if (alphaTest.preserveDestinationAlpha)
    {
        uint32_t existing = 0u;
        std::memcpy(&existing, gs->m_vram + off, 4);
        pixel = (pixel & 0x00FFFFFFu) | (existing & 0xFF000000u);
    }

    if (g195ForegroundTraceTarget(ctx, x, y))
    {
        uint32_t before = 0u;
        std::memcpy(&before, gs->m_vram + off, 4);
        g195LogForegroundPixel(ctx, x, y, off, bytesPerPixel,
                               static_cast<uint32_t>(gs->m_prim.type),
                               gs->m_prim.tme, gs->m_prim.abe,
                               gs->m_prim.fst, gs->m_prim.ctxt, gs->m_pabe,
                               before, srcR, srcG, srcB, srcA, pixel);
    }

    if (g43FacePixelTraceEnabled() &&
        ctx.frame.fbp == 0x139u &&
        gs->m_prim.tme &&
        x >= 344 && x <= 412 &&
        y >= 76 && y <= 144 &&
        ((x & 3) == 0) &&
        ((y & 3) == 0))
    {
        static std::atomic<uint32_t> s_g43FacePx{0};
        const uint32_t n = s_g43FacePx.fetch_add(1u, std::memory_order_relaxed);
        if (n < 4000u)
        {
            std::fprintf(stderr,
                "[G43:px] n=%u xy=(%d,%d) prim=%u tme=%u abe=%u fst=%u iip=%u "
                "tbp=0x%x tpsm=0x%x cbp=0x%x csa=%u src=%02x,%02x,%02x,%02x "
                "final=0x%08x alpha=0x%llx test=0x%llx zbuf=0x%llx\n",
                n, x, y,
                static_cast<uint32_t>(gs->m_prim.type),
                static_cast<uint32_t>(gs->m_prim.tme),
                static_cast<uint32_t>(gs->m_prim.abe),
                static_cast<uint32_t>(gs->m_prim.fst),
                static_cast<uint32_t>(gs->m_prim.iip),
                ctx.tex0.tbp0,
                static_cast<uint32_t>(ctx.tex0.psm),
                ctx.tex0.cbp,
                static_cast<uint32_t>(ctx.tex0.csa),
                srcR, srcG, srcB, a,
                pixel,
                static_cast<unsigned long long>(ctx.alpha),
                static_cast<unsigned long long>(ctx.test),
                static_cast<unsigned long long>(ctx.zbuf));
        }
    }

    f50_12_log_framebuffer_commit(ctx, x, y, off, bytesPerPixel, pixel,
                                  gs->m_prim.type, gs->m_prim.tme, gs->m_prim.abe,
                                  gs->m_prim.fst, gs->m_prim.ctxt);
    std::memcpy(gs->m_vram + off, &pixel, 4);
}

// F62 (PR #132): texel reads now go through the GSMem swizzle module. GSMem's
// address math is byte-identical to the local GSPSMCT32/GSPSMT4 helpers (proven for
// 32-bit and the index formats) and is the canonical swizzle for the 16-bit family
// (which the old upload wrote linearly — see processImageData). GSMem takes the
// buffer width in PAGES, so PSMT4's 128-wide page needs tbw>>1; the 32/24/16-bit and
// the *H index formats (64-wide page) use tbw directly.
uint32_t GSRasterizer::readTexelPSMCT32(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;
    return GSMem::ReadCT32(gs->m_vram, tbp0, tbw,
                           static_cast<uint32_t>(texU), static_cast<uint32_t>(texV));
}

uint32_t GSRasterizer::readTexelPSMCT16(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;
    const uint8_t psm = gs->activeContext().tex0.psm;
    uint32_t raw;
    switch (psm)
    {
    case GS_PSM_CT16S: raw = GSMem::ReadCT16S(gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    case GS_PSM_Z16:   raw = GSMem::ReadZ16  (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    case GS_PSM_Z16S:  raw = GSMem::ReadZ16S (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    default:           raw = GSMem::ReadCT16 (gs->m_vram, tbp0, tbw, static_cast<uint32_t>(texU), static_cast<uint32_t>(texV)); break;
    }
    return decodePSMCT16(static_cast<uint16_t>(raw));
}

uint32_t GSRasterizer::readTexelPSMT4(GS *gs, uint32_t tbp0, uint32_t tbw, int texU, int texV)
{
    if (tbw == 0)
        tbw = 1;

    const uint8_t psm = gs->activeContext().tex0.psm;
    const uint32_t ux = static_cast<uint32_t>(texU);
    const uint32_t uy = static_cast<uint32_t>(texV);

    // T4HH/T4HL hide the 4-bit index in the upper byte of a 32-bit word (F37). GSMem's
    // P4HH/P4HL readers (BitOffset 28/24 over the C32 page layout) extract exactly that
    // nibble — byte-identical to the old upper-byte read.
    if (psm == GS_PSM_T4HH)
        return GSMem::ReadP4HH(gs->m_vram, tbp0, tbw, ux, uy);
    if (psm == GS_PSM_T4HL)
        return GSMem::ReadP4HL(gs->m_vram, tbp0, tbw, ux, uy);

    // Plain PSMT4: 128-wide page → tbw>>1.
    const uint32_t pageBwT4 = ((tbw >> 1u) != 0u) ? (tbw >> 1u) : 1u;
    return GSMem::ReadP4(gs->m_vram, tbp0, pageBwT4, ux, uy);
}

uint32_t GSRasterizer::lookupCLUT(GS *gs,
                                  uint8_t index,
                                  uint32_t cbp,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, csm, csa, sourcePsm);
    const uint32_t clutWidth = (gs->m_texclut.cbw != 0u) ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;
    const uint32_t clutX = static_cast<uint32_t>(gs->m_texclut.cou) + (clutIndex & 0x0Fu);
    const uint32_t clutY = static_cast<uint32_t>(gs->m_texclut.cov) + (clutIndex >> 4);

    // F62 (PR #132): CLUT fetch via GSMem (CT32 == local addrPSMCT32, byte-identical;
    // 16-bit uses the canonical swizzle). NOTE: unlike the PR's hunk, the 16-bit CLUT
    // is still run through decodePSMCT16 (the PR dropped that, which would feed raw
    // 5-5-5-1 into applyTexa).
    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(gs->m_texa, cpsm, GSMem::ReadCT32(gs->m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(gs->m_texa, cpsm, GSMem::ReadCT24(gs->m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(gs->m_texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16(gs->m_vram, cbp, clutWidth, clutX, clutY))));
    case GS_PSM_CT16S:
        return applyTexa(gs->m_texa, cpsm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadCT16S(gs->m_vram, cbp, clutWidth, clutX, clutY))));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSRasterizer::sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = gs->activeContext();
    const auto &tex = ctx.tex0;

    int texW = 1 << tex.tw;
    int texH = 1 << tex.th;

    float texUf, texVf;
    if (gs->m_prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto f50_12_log_pal_sample = [&](int sampleU, int sampleV, uint32_t idx, uint32_t color) {
        if (!f50_12_trace_enabled())
            return;

        static std::atomic<uint64_t> s_total{0};
        static std::atomic<uint64_t> s_bad{0};
        static std::atomic<uint64_t> s_title{0};
        static std::atomic<uint64_t> s_manager{0};
        static std::atomic<uint64_t> s_other{0};

        const bool bad = tex.tbp0 == 0x2580u && tex.cbp == 0x2980u && tex.cpsm == GS_PSM_CT16;
        const bool title = tex.tbp0 == 0x1A00u && tex.cbp == 0x3FE0u;
        const bool manager = tex.tbp0 == 0x2720u || tex.tbp0 == 0x2820u ||
                             tex.tbp0 == 0x29A0u || tex.tbp0 == 0x2F20u ||
                             tex.tbp0 == 0x3420u;
        const char *bucket = "other";
        std::atomic<uint64_t> *bucketCounter = &s_other;
        if (bad)
        {
            bucket = "bad2580";
            bucketCounter = &s_bad;
        }
        else if (title)
        {
            bucket = "title";
            bucketCounter = &s_title;
        }
        else if (manager)
        {
            bucket = "manager";
            bucketCounter = &s_manager;
        }

        const uint64_t n = s_total.fetch_add(1u, std::memory_order_relaxed) + 1ull;
        const uint64_t bucketN = bucketCounter->fetch_add(1u, std::memory_order_relaxed) + 1ull;
        const bool shouldLog = bucketN <= 32ull ||
                               (bad && (bucketN % 1048576ull) == 0ull) ||
                               (manager && (bucketN % 65536ull) == 0ull) ||
                               ((n % 1048576ull) == 0ull);
        if (!shouldLog)
            return;

        std::fprintf(stderr,
                     "[F512:samples] n=%llu bucket=%s bucketN=%llu prim=%u tme=%u abe=%u fst=%u ctxt=%u fbp=0x%x fbw=%u fpsm=0x%x tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u cld=%u texclut(cbw=%u,cou=%u,cov=%u) uv=(%d,%d) idx=%u color=0x%08x\n",
                     static_cast<unsigned long long>(n),
                     bucket,
                     static_cast<unsigned long long>(bucketN),
                     static_cast<uint32_t>(gs->m_prim.type),
                     static_cast<uint32_t>(gs->m_prim.tme),
                     static_cast<uint32_t>(gs->m_prim.abe),
                     static_cast<uint32_t>(gs->m_prim.fst),
                     static_cast<uint32_t>(gs->m_prim.ctxt),
                     ctx.frame.fbp,
                     static_cast<uint32_t>(ctx.frame.fbw),
                     static_cast<uint32_t>(ctx.frame.psm),
                     static_cast<unsigned long long>(f50_12_tex0_raw(tex)),
                     tex.tbp0,
                     static_cast<uint32_t>(tex.tbw),
                     static_cast<uint32_t>(tex.psm),
                     static_cast<uint32_t>(tex.tw),
                     static_cast<uint32_t>(tex.th),
                     static_cast<uint32_t>(tex.tcc),
                     static_cast<uint32_t>(tex.tfx),
                     tex.cbp,
                     static_cast<uint32_t>(tex.cpsm),
                     static_cast<uint32_t>(tex.csm),
                     static_cast<uint32_t>(tex.csa),
                     static_cast<uint32_t>(tex.cld),
                     static_cast<uint32_t>(gs->m_texclut.cbw),
                     static_cast<uint32_t>(gs->m_texclut.cou),
                     static_cast<uint32_t>(gs->m_texclut.cov),
                     sampleU,
                     sampleV,
                     idx,
                     color);
    };

    const uint8_t wrapU = static_cast<uint8_t>(ctx.clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((ctx.clamp >> 2u) & 0x3u);
    const int minU = static_cast<int>((ctx.clamp >> 4u) & 0x3FFu);
    const int maxU = static_cast<int>((ctx.clamp >> 14u) & 0x3FFu);
    const int minV = static_cast<int>((ctx.clamp >> 24u) & 0x3FFu);
    const int maxV = static_cast<int>((ctx.clamp >> 34u) & 0x3FFu);

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, wrapU, texW, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, wrapV, texH, minV, maxV);

        if (tex.psm == GS_PSM_CT32 || tex.psm == GS_PSM_CT24)
            return applyTexa(gs->m_texa, tex.psm, readTexelPSMCT32(gs, tex.tbp0, tex.tbw, sampleU, sampleV));

        if (tex.psm == GS_PSM_CT16 || tex.psm == GS_PSM_CT16S)
            return applyTexa(gs->m_texa, tex.psm, readTexelPSMCT16(gs, tex.tbp0, tex.tbw, sampleU, sampleV));

        if (tex.psm == GS_PSM_T4 ||
            tex.psm == GS_PSM_T4HL ||
            tex.psm == GS_PSM_T4HH)
        {
            // G5: one-shot decode of DC2's 4HH debug-font atlas. This separates
            // VRAM/CLUT extraction defects from coordinate wrapping defects.
            if (tex.psm == GS_PSM_T4HH && tex.tbp0 == 0x1A00u && envFlagEnabled("DC2_DUMP_FONT"))
            {
                static std::atomic<uint32_t> s_dumped4hh{0};
                if (s_dumped4hh.fetch_add(1u, std::memory_order_relaxed) == 0u)
                {
                    const int dw = 1 << tex.tw;
                    const int dh = 1 << tex.th;
                    char path[128];
                    std::snprintf(path, sizeof(path), "captures/font4hh_%x.ppm", tex.tbp0);
                    FILE *fp = std::fopen(path, "wb");
                    if (fp)
                    {
                        std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                        for (int yy = 0; yy < dh; ++yy)
                        {
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                const uint8_t ix = static_cast<uint8_t>(
                                    GSMem::ReadP4HH(gs->m_vram, tex.tbp0, tex.tbw,
                                                   static_cast<uint32_t>(xx),
                                                   static_cast<uint32_t>(yy)));
                                const uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm,
                                                              tex.csm, tex.csa, tex.psm);
                                const uint32_t alpha = std::min<uint32_t>((c >> 24) & 0xFFu, 0x80u);
                                const uint8_t rgb[3] = {
                                    static_cast<uint8_t>(((c & 0xFFu) * alpha) / 0x80u),
                                    static_cast<uint8_t>((((c >> 8) & 0xFFu) * alpha) / 0x80u),
                                    static_cast<uint8_t>((((c >> 16) & 0xFFu) * alpha) / 0x80u)
                                };
                                std::fwrite(rgb, 1, 3, fp);
                            }
                        }
                        std::fclose(fp);
                    }

                    const char *vramPath = "captures/font4hh_vram.bin";
                    FILE *vramFp = std::fopen(vramPath, "wb");
                    if (vramFp)
                    {
                        std::fwrite(gs->m_vram, 1, gs->m_vramSize, vramFp);
                        std::fclose(vramFp);
                    }

                    std::fprintf(stderr,
                                 "[G5:font4hh] path=%s vram=%s tbp=0x%x tbw=%u psm=0x%x size=%dx%d "
                                 "cbp=0x%x cpsm=0x%x csa=%u csm=%u cld=%u clamp=0x%llx\n",
                                 path, vramPath, tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                 static_cast<uint32_t>(tex.psm), dw, dh, tex.cbp,
                                 static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                                 static_cast<uint32_t>(tex.csm), static_cast<uint32_t>(tex.cld),
                                 static_cast<unsigned long long>(ctx.clamp));

                    for (uint32_t bw = 1u; bw <= 16u; ++bw)
                    {
                        uint32_t nonzero = 0u;
                        uint32_t hash = 2166136261u;
                        for (int yy = 0; yy < dh; ++yy)
                        {
                            for (int xx = 0; xx < dw; ++xx)
                            {
                                const uint8_t ix = static_cast<uint8_t>(
                                    GSMem::ReadP4HH(gs->m_vram, tex.tbp0, bw,
                                                   static_cast<uint32_t>(xx),
                                                   static_cast<uint32_t>(yy)));
                                nonzero += (ix != 0u) ? 1u : 0u;
                                hash = (hash ^ ix) * 16777619u;
                            }
                        }
                        std::fprintf(stderr, "[G5:font4hh-bw] bw=%u nonzero=%u hash=0x%08x%s\n",
                                     bw, nonzero, hash, (bw == tex.tbw) ? " active" : "");
                    }

                    for (uint32_t csa = 0u; csa < 32u; ++csa)
                    {
                        std::fprintf(stderr, "[G5:font4hh-csa] csa=%u", csa);
                        for (uint32_t ix = 0u; ix < 16u; ++ix)
                        {
                            const uint32_t c = lookupCLUT(gs, static_cast<uint8_t>(ix), tex.cbp,
                                                          tex.cpsm, tex.csm,
                                                          static_cast<uint8_t>(csa), tex.psm);
                            std::fprintf(stderr, " %08x", c);
                        }
                        std::fputc('\n', stderr);
                    }
                }
            }

            uint32_t idx = readTexelPSMT4(gs, tex.tbp0, tex.tbw, sampleU, sampleV);
            uint32_t clutColor = lookupCLUT(gs, static_cast<uint8_t>(idx), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            f50_12_log_pal_sample(sampleU, sampleV, idx, clutColor);
            if (renderQualityTraceEnabled())
            {
                // F50.10: capture paletted samples that are NOT the dominant tbp=0x2580
                // material, to see whether the data-backed streamed textures (0x2820/
                // 0x29a0/0x2f20, proven present in VRAM) are ever actually drawn.
                if (tex.tbp0 != 0x2580u)
                {
                    static std::atomic<int> s_f510other{0};
                    const int o = s_f510other.fetch_add(1, std::memory_order_relaxed);
                    if (o < 48)
                        std::fprintf(stderr,
                                     "[F510:othertex] o=%d psm=0x%x tbp=0x%x tbw=%u cbp=0x%x cpsm=0x%x cld=%u idx=%u color=0x%08x\n",
                                     o, (uint32_t)tex.psm, tex.tbp0, (uint32_t)tex.tbw,
                                     tex.cbp, (uint32_t)tex.cpsm, (uint32_t)tex.cld, idx, clutColor);
                }
                static std::atomic<int> s_f508T4{0};
                const int k = s_f508T4.fetch_add(1, std::memory_order_relaxed);
                if (k < 96)
                {
                    const uint32_t cw = gs->m_texclut.cbw ? static_cast<uint32_t>(gs->m_texclut.cbw) : 1u;
                    const uint32_t coff = GSPSMCT32::addrPSMCT32(tex.cbp, cw,
                                                                 static_cast<uint32_t>(gs->m_texclut.cou),
                                                                 gs->m_texclut.cov);
                    uint32_t cbase = 0u;
                    if (coff + 4u <= gs->m_vramSize)
                        std::memcpy(&cbase, gs->m_vram + coff, 4);
                    std::fprintf(stderr,
                                 "[F508:t4clut] k=%d psm=0x%x tbp=0x%x tbw=%u cbp=0x%x cpsm=0x%x csa=%u cld=%u idx=%u color=0x%08x clutbase@0x%x=0x%08x\n",
                                 k, static_cast<uint32_t>(tex.psm), tex.tbp0, static_cast<uint32_t>(tex.tbw),
                                 tex.cbp, static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                                 static_cast<uint32_t>(tex.cld),
                                 idx, clutColor, coff, cbase);
                    // F50.9: one-time dump of BOTH GS contexts' TEX0 + active context, to
                    // check whether the texture bind targeted one context while the draw
                    // samples the (stale) other context (PRIM.CTXT mismatch).
                    if (k == 0)
                    {
                        const auto &t0 = gs->m_ctx[0].tex0;
                        const auto &t1 = gs->m_ctx[1].tex0;
                        std::fprintf(stderr,
                                     "[F509:ctxdump] primCtxt=%u activeIsT0=%d "
                                     "ctx0(tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u) "
                                     "ctx1(tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x cld=%u)\n",
                                     gs->m_prim.ctxt ? 1u : 0u, (&ctx == &gs->m_ctx[0]) ? 1 : 0,
                                     t0.tbp0, (unsigned)t0.tbw, (unsigned)t0.psm, t0.cbp, (unsigned)t0.cpsm, (unsigned)t0.cld,
                                     t1.tbp0, (unsigned)t1.tbw, (unsigned)t1.psm, t1.cbp, (unsigned)t1.cpsm, (unsigned)t1.cld);
                    }
                    // F50.9: one-time dump of the actual upload destinations seen in the
                    // BITBLTBUF histogram (textures @0x2820/0x29a0/0x2f20, CLUTs @0x3fb4..0x3fbc)
                    // to confirm the uploaded data physically landed, vs the (empty) pages the
                    // draws reference (tbp=0x2580 cbp=0x2980).
                    if (k == 60)
                    {
                        auto dwAt = [&](uint32_t blk) -> uint32_t {
                            const uint32_t byteOff = blk * 256u;
                            uint32_t w = 0u;
                            if (byteOff + 4u <= gs->m_vramSize)
                                std::memcpy(&w, gs->m_vram + byteOff, 4);
                            return w;
                        };
                        // Count nonzero bytes per 64KB region across all of VRAM so we can see
                        // WHERE uploaded image data actually landed (vs the empty draw pages).
                        auto nzRegion = [&](uint32_t startByte, uint32_t len) -> uint32_t {
                            uint32_t nz = 0u;
                            const uint32_t end = (startByte + len <= gs->m_vramSize) ? (startByte + len) : gs->m_vramSize;
                            for (uint32_t i = startByte; i < end; ++i)
                                if (gs->m_vram[i] != 0u) ++nz;
                            return nz;
                        };
                        std::fprintf(stderr,
                                     "[F508:vramdump] tex@0x2820=0x%08x tex@0x29a0=0x%08x tex@0x2f20=0x%08x "
                                     "clut@0x3fb4=0x%08x clut@0x3fb8=0x%08x clut@0x3fbc=0x%08x clut@0x3fe0=0x%08x "
                                     "drawTex@0x2580=0x%08x drawClut@0x2980=0x%08x vramSize=0x%x\n",
                                     dwAt(0x2820u), dwAt(0x29a0u), dwAt(0x2f20u),
                                     dwAt(0x3fb4u), dwAt(0x3fb8u), dwAt(0x3fbcu), dwAt(0x3fe0u),
                                     dwAt(0x2580u), dwAt(0x2980u), gs->m_vramSize);
                        // 0x100000-byte (1MB) buckets across 4MB VRAM.
                        std::fprintf(stderr,
                                     "[F508:vramnz] nz[0-1MB]=%u nz[1-2MB]=%u nz[2-3MB]=%u nz[3-4MB]=%u "
                                     "nz@tex0x258000(64K)=%u nz@tex0x282000(64K)=%u nz@clut0x3fb000(16K)=%u\n",
                                     nzRegion(0x000000u, 0x100000u), nzRegion(0x100000u, 0x100000u),
                                     nzRegion(0x200000u, 0x100000u), nzRegion(0x300000u, 0x100000u),
                                     nzRegion(0x258000u, 0x10000u), nzRegion(0x282000u, 0x10000u),
                                     nzRegion(0x3fb000u, 0x4000u));
                    }
                }
            }
            return clutColor;
        }

        if (tex.psm == GS_PSM_T8)
        {
            if (tex.tbw == 0)
                return 0xFFFF00FFu;
            // G2: one-shot dump of the decoded T8 atlas as the sampler sees it (full
            // tw/th, GSMem::ReadP8 -> lookupCLUT). DC2_DUMP_FONT=1. Reveals whether the
            // CT32-alias upload + T8 read recover a coherent glyph atlas, or garbage.
            {
                static const bool s_dumpFont = []() {
                    const char *e = std::getenv("DC2_DUMP_FONT");
                    return e && e[0] != '0';
                }();
                if (s_dumpFont)
                {
                    static std::atomic<uint32_t> s_dumped{0};
                    // dump each distinct font-ish tbp once
                    static std::atomic<uint32_t> s_seen2720{0}, s_seen2f20{0}, s_seen3220{0}, s_seen2aa0{0};
                    std::atomic<uint32_t> *seen =
                        (tex.tbp0 == 0x2720u) ? &s_seen2720 :
                        (tex.tbp0 == 0x2f20u) ? &s_seen2f20 :
                        (tex.tbp0 == 0x2aa0u) ? &s_seen2aa0 :
                        (tex.tbp0 == 0x3220u) ? &s_seen3220 : nullptr;
                    if (seen && seen->fetch_add(1u, std::memory_order_relaxed) == 0u && s_dumped.load() < 6u)
                    {
                        s_dumped.fetch_add(1u, std::memory_order_relaxed);
                        const int dw = 1 << tex.tw, dh = 1 << tex.th;
                        for (uint32_t bwv = 1u; bwv <= 4u; ++bwv)
                        {
                            char path[128];
                            std::snprintf(path, sizeof(path), "captures/font_%x_bw%u.ppm", tex.tbp0, bwv);
                            FILE *fp = std::fopen(path, "wb");
                            if (!fp) continue;
                            std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                            for (int yy = 0; yy < dh; ++yy)
                                for (int xx = 0; xx < dw; ++xx)
                                {
                                    uint8_t ix = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, bwv,
                                                     static_cast<uint32_t>(xx), static_cast<uint32_t>(yy)));
                                    uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                                    uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                                       static_cast<uint8_t>((c >> 8) & 0xFF),
                                                       static_cast<uint8_t>((c >> 16) & 0xFF) };
                                    std::fwrite(rgb, 1, 3, fp);
                                }
                            std::fclose(fp);
                        }
                        {
                            std::fprintf(stderr, "[G2:fontdump] wrote %x bw-sweep dw=%d dh=%d tbw=%u cbp=0x%x cpsm=%u csa=%u\n",
                                tex.tbp0, dw, dh, tex.tbw, tex.cbp, (unsigned)tex.cpsm, (unsigned)tex.csa);
                        }
                        // Variant 2: read the page as RAW LINEAR bytes (base + y*dw + x),
                        // i.e. exactly how a flat host buffer would lay it out. If THIS is
                        // legible, the data landed linearly and the T8 swizzle read is wrong.
                        char path2[128];
                        std::snprintf(path2, sizeof(path2), "captures/font_%x_linear.ppm", tex.tbp0);
                        FILE *fp2 = std::fopen(path2, "wb");
                        if (fp2)
                        {
                            const uint32_t base = tex.tbp0 * 256u;
                            std::fprintf(fp2, "P6\n%d %d\n255\n", dw, dh);
                            for (int yy = 0; yy < dh; ++yy)
                                for (int xx = 0; xx < dw; ++xx)
                                {
                                    const uint32_t o = base + static_cast<uint32_t>(yy * dw + xx);
                                    uint8_t ix = (o < gs->m_vramSize) ? gs->m_vram[o] : 0u;
                                    uint32_t c = lookupCLUT(gs, ix, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
                                    uint8_t rgb[3] = { static_cast<uint8_t>(c & 0xFF),
                                                       static_cast<uint8_t>((c >> 8) & 0xFF),
                                                       static_cast<uint8_t>((c >> 16) & 0xFF) };
                                    std::fwrite(rgb, 1, 3, fp2);
                                }
                            std::fclose(fp2);
                        }
                        // G3: raw page-region snapshot at SAMPLE time. Compare vs the
                        // post-upload snapshot ([G3:postup]) to detect an overwrite of
                        // the font page between upload and sampling. DC2_DUMP_VRAMRGN=1.
                        {
                            const char *er = std::getenv("DC2_DUMP_VRAMRGN");
                            if (er && er[0] != '0')
                            {
                                const uint32_t rbase = tex.tbp0 * 256u;
                                const uint32_t rlen = 0x40000u;
                                if (rbase + rlen <= gs->m_vramSize)
                                {
                                    char pr[128];
                                    std::snprintf(pr, sizeof(pr), "captures/vram_atsample_%x.bin", tex.tbp0);
                                    FILE *fpr = std::fopen(pr, "wb");
                                    if (fpr) { std::fwrite(gs->m_vram + rbase, 1, rlen, fpr); std::fclose(fpr); }
                                    std::fprintf(stderr, "[G3:atsample] wrote %s\n", pr);
                                }
                            }
                        }
                    }
                }
            }
            // [F51.3] env-gated experiment: override the PSMT8 sample stride. DC2 uploads its
            // manager T8 textures via a CT32 alias (BITBLT dbw=2) but declares TEX0.TBW=6
            // (< TW-width/64=8); the upload-consistent stride is dbw*2=4. Default OFF (no behavior
            // change) — set DC2_T8_ALIAS_TBW=4 to test whether this makes the dungeon non-black.
            uint32_t t8tbw = tex.tbw;
            {
                static const int s_aliasTbw = []() {
                    const char *e = std::getenv("DC2_T8_ALIAS_TBW");
                    return e ? std::atoi(e) : 0;
                }();
                if (s_aliasTbw > 0)
                    t8tbw = static_cast<uint32_t>(s_aliasTbw);
            }
            uint32_t off = GSPSMT8::addrPSMT8(tex.tbp0, t8tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
            if (renderQualityTraceEnabled())
            {
                static std::atomic<int> s_t8Probe{0};
                if (s_t8Probe.fetch_add(1, std::memory_order_relaxed) < 5)
                {
                    uint8_t vb = (off < gs->m_vramSize) ? gs->m_vram[off] : 0xFFu;
                    std::fprintf(stderr, "[F37:t8-sample] tbp=0x%x tbw=%u uv=(%d,%d) off=0x%x vram_byte=0x%02x cbp=0x%x cpsm=%u csm=%u\n",
                        tex.tbp0, tex.tbw, sampleU, sampleV, off, vb, tex.cbp, tex.cpsm, tex.csm);
                }
                // G2 self-check: is GSMem::ReadP8 byte-identical to the local addrPSMT8 read?
                // If g!=l ever fires, the swizzles differ; if never, the read fix is a no-op
                // and the corruption is upstream (CT32-alias upload / wrong format).
                {
                    static std::atomic<int> s_g2chk{0};
                    static std::atomic<int> s_g2mismatch{0};
                    const uint32_t pageBwT8 = ((t8tbw >> 1u) != 0u) ? (t8tbw >> 1u) : 1u;
                    uint8_t lb = (off < gs->m_vramSize) ? gs->m_vram[off] : 0xFFu;
                    uint8_t gb = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                                     static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
                    if (lb != gb) s_g2mismatch.fetch_add(1, std::memory_order_relaxed);
                    int n = s_g2chk.fetch_add(1, std::memory_order_relaxed);
                    if (n < 16 || (lb != gb && n < 4096 && (n % 64) == 0))
                        std::fprintf(stderr, "[G2:t8cmp] n=%d tbp=0x%x tbw=%u uv=(%d,%d) local=0x%02x gsmem=0x%02x %s mism=%d\n",
                            n, tex.tbp0, t8tbw, sampleU, sampleV, lb, gb, (lb==gb?"EQ":"DIFF"),
                            s_g2mismatch.load(std::memory_order_relaxed));
                }
                // F51: one-shot per-TBW scan for the manager texture. The CT32-alias
                // upload uses BITBLTBUF.DBW; the sampler reads addrPSMT8 with TEX0.TBW.
                // Probe which TBW (if any) makes the sampled texel land on uploaded data,
                // to decide whether the upload page-stride (DBW) or the sample TBW is off.
                if (tex.tbp0 == 0x2720u)
                {
                    static std::atomic<int> s_t8tbw{0};
                    if (s_t8tbw.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        char line[384];
                        int n = std::snprintf(line, sizeof(line),
                            "[F51:tbwscan] uv=(%d,%d) sampleTbw=%u:",
                            sampleU, sampleV, static_cast<uint32_t>(tex.tbw));
                        for (uint32_t tw = 2u; tw <= 8u && n > 0 && n < (int)sizeof(line); ++tw)
                        {
                            uint32_t o = GSPSMT8::addrPSMT8(tex.tbp0, tw,
                                static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
                            uint8_t b = (o < gs->m_vramSize) ? gs->m_vram[o] : 0xFFu;
                            n += std::snprintf(line + n, sizeof(line) - n,
                                " tbw%u=0x%02x@0x%x", tw, b, o);
                        }
                        // locate the first nonzero byte in the CT32-written window (pages 0x139..0x141)
                        uint32_t firstNz = 0u; uint8_t firstNzB = 0u;
                        for (uint32_t a = 0x272000u; a < 0x282000u && a < gs->m_vramSize; ++a)
                            if (gs->m_vram[a] != 0u) { firstNz = a; firstNzB = gs->m_vram[a]; break; }
                        // linear nonzero count of the whole 64KB page at SAMPLE time. Compare vs
                        // upload's nzT8=62381: ~62000 => data present (read at wrong addr = swizzle bug);
                        // ~0 => data overwritten (RTT/clear corruption).
                        uint32_t pageNz = 0u;
                        for (uint32_t a = 0x272000u; a < 0x282000u && a < gs->m_vramSize; ++a)
                            if (gs->m_vram[a] != 0u) ++pageNz;
                        std::fprintf(stderr, "%s | firstNz@0x%x=0x%02x pageNz=%u\n", line, firstNz, firstNzB, pageNz);
                    }
                }
            }
            // G2: route the PSMT8 read through the byte-exact GSMem swizzle (like the F62
            // T8 *upload* GSMem::WriteP8, and the T4/CT32/CT16 reads). The old local
            // GSPSMT8::addrPSMT8 is NOT byte-identical to GSMem's P8 page/column table, so
            // textures DC2 uploads via a CT32-alias BITBLT (dpsm=CT32, written by byte-exact
            // GSMem::WriteCT32) and then samples as T8 came back garbled — this is the
            // menu/dialog/HUD font-atlas corruption (fonts are T8: tbp 0x2720/0x2f20, cbp
            // CT32). GSMem buffer width is in PAGES (T8 page = 128 wide) → tbw>>1, symmetric
            // with the upload. Default ON; DC2_T8_GSMEM=0 falls back to the old path for A/B.
            static const bool s_t8Gsmem = []() {
                const char *e = std::getenv("DC2_T8_GSMEM");
                return !(e && (e[0] == '0'));
            }();
            uint8_t idx;
            if (s_t8Gsmem)
            {
                const uint32_t pageBwT8 = ((t8tbw >> 1u) != 0u) ? (t8tbw >> 1u) : 1u;
                idx = static_cast<uint8_t>(GSMem::ReadP8(gs->m_vram, tex.tbp0, pageBwT8,
                                                         static_cast<uint32_t>(sampleU),
                                                         static_cast<uint32_t>(sampleV)));
            }
            else
            {
                if (off >= gs->m_vramSize)
                    return 0xFFFF00FFu;
                idx = gs->m_vram[off];
            }
            uint32_t clutColor = lookupCLUT(gs, idx, tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
            f50_12_log_pal_sample(sampleU, sampleV, idx, clutColor);
            // [F51.5] decisive: the displayed map texture is tbp=0x3220. Log its actual idx/color.
            // idx==0 -> texture reads 0 at the sampled UV (UV/overwrite); idx!=0 && color==0 -> CLUT
            // lookup broken (same class as 0x1a00 with the never-uploaded 0x3fe0 CLUT).
            if (renderQualityTraceEnabled() && tex.tbp0 == 0x3220u)
            {
                static std::atomic<int> s_f515{0};
                if (s_f515.fetch_add(1, std::memory_order_relaxed) < 12)
                    std::fprintf(stderr,
                        "[F51.5:mapsample] tbp=0x3220 tbw=%u uv=(%d,%d) off=0x%x byte=0x%02x idx=%u "
                        "cbp=0x%x cpsm=%u csa=%u cld=%u color=0x%08x\n",
                        static_cast<uint32_t>(tex.tbw), sampleU, sampleV, off,
                        (off < gs->m_vramSize ? gs->m_vram[off] : 0xFFu), static_cast<uint32_t>(idx),
                        tex.cbp, static_cast<uint32_t>(tex.cpsm), static_cast<uint32_t>(tex.csa),
                        static_cast<uint32_t>(tex.cld), clutColor);
            }
            return clutColor;
        }

        // F62 (PR #132): formats the old sampler returned magenta for. GSMem decodes
        // them; DC2 is not known to source these as textures, so this only adds
        // coverage (no change for the CT32/CT16/T4/T8 paths above).
        if (tex.psm == GS_PSM_T8H)
        {
            const uint32_t idx = GSMem::ReadP8H(gs->m_vram, tex.tbp0, tex.tbw,
                                                static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV));
            return lookupCLUT(gs, static_cast<uint8_t>(idx), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        }
        if (tex.psm == GS_PSM_Z32)
            return applyTexa(gs->m_texa, tex.psm, GSMem::ReadZ32(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
        if (tex.psm == GS_PSM_Z24)
            return applyTexa(gs->m_texa, tex.psm, GSMem::ReadZ24(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)));
        if (tex.psm == GS_PSM_Z16)
            return applyTexa(gs->m_texa, tex.psm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadZ16(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)))));
        if (tex.psm == GS_PSM_Z16S)
            return applyTexa(gs->m_texa, tex.psm, decodePSMCT16(static_cast<uint16_t>(GSMem::ReadZ16S(gs->m_vram, tex.tbp0, tex.tbw, static_cast<uint32_t>(sampleU), static_cast<uint32_t>(sampleV)))));

        return 0xFFFF00FFu;
    };

    // G45 experiment: the costume head's detailed face texture is point-sampled at native
    // 512-wide res, so its dark detail texels (nose/brow/freckle) pop as hard dots vs PCSX2's
    // upscaled+filtered smooth face. Optionally force bilinear for the costume model RTT only.
    static const bool s_g45Bilinear = (std::getenv("DC2_G45_BILINEAR") != nullptr);
    const bool g45ForceLinear = s_g45Bilinear && gs->activeContext().frame.fbp == 0x139u;

    // G51: minification box-filter for the costume model RTT. The face texture (0x3480) is a
    // detailed 128x128 with fine dark feature texels (brows/eye-liner/nostril/lip/eye-socket
    // shadow); it is minified ~2.5x onto the ~50px on-screen face. Point sampling (and even a 2x2
    // bilinear tap) skips texels between samples -> the dark features ALIAS into scattered dark
    // "speckle" dots, vs PCSX2's internal-res-upscaled smooth reference. Average a (2k+1)^2 texel
    // box around the sample to approximate area minification. Scoped to fbp==0x139; env-gated
    // DC2_G51_MINFILT = box radius k in texels (0 = off). Decisive root-cause test + the fix.
    static const int s_g51MinFilt = []() -> int {
        const char *e = std::getenv("DC2_G51_MINFILT");
        return e ? std::atoi(e) : 0;
    }();
    if (s_g51MinFilt > 0 && gs->activeContext().frame.fbp == 0x139u)
    {
        const int cu = static_cast<int>(std::floor(texUf));
        const int cv = static_cast<int>(std::floor(texVf));
        int rsum = 0, gsum = 0, bsum = 0, asum = 0, cnt = 0;
        for (int dv = -s_g51MinFilt; dv <= s_g51MinFilt; ++dv)
            for (int du = -s_g51MinFilt; du <= s_g51MinFilt; ++du)
            {
                const uint32_t c = samplePoint(cu + du, cv + dv);
                rsum += static_cast<int>(c & 0xFFu);
                gsum += static_cast<int>((c >> 8) & 0xFFu);
                bsum += static_cast<int>((c >> 16) & 0xFFu);
                asum += static_cast<int>((c >> 24) & 0xFFu);
                ++cnt;
            }
        const uint32_t rr = static_cast<uint32_t>(rsum / cnt);
        const uint32_t gg = static_cast<uint32_t>(gsum / cnt);
        const uint32_t bb = static_cast<uint32_t>(bsum / cnt);
        const uint32_t aa = static_cast<uint32_t>(asum / cnt);
        return rr | (gg << 8) | (bb << 16) | (aa << 24);
    }

    if (!g45ForceLinear && !tex1UsesLinearFilter(ctx.tex1))
    {
        const uint32_t sampled = samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
        if (renderQualityTraceEnabled())
            f508LogSample(tex.psm, sampled);
        return sampled;
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    const uint32_t sampledLinear = static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
    if (renderQualityTraceEnabled())
        f508LogSample(tex.psm, sampledLinear);
    return sampledLinear;
}