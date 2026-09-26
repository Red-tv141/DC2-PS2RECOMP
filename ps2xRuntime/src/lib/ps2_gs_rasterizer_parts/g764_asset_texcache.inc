// Offline asset membership is proved from complete raw GS byte witnesses, never
// from an address, filename, short hash, or a previous frame's palette identity.
static dc2::assetcache::Image g764FindAssetTexture(
    GSRasterizer* self, GS* gs, uint8_t* vram, size_t vramSize,
    const G144Entry& e, int width, int height, bool dynamicSource)
{
    const auto* cache = dc2::assetcache::runtimeCache();
    if (!cache || dynamicSource || g600TexPaintOn()) return {};
    const auto& t = e.ctx.tex0;
    // The offline v1 decoder intentionally covers CT32 and CSM1/CSA0 paletted
    // CT32 only. TEXA is an identity for these formats. Every other representation
    // (including high-bit Z aliases and palette animation windows) keeps fallback.
    if (t.psm != GS_PSM_CT32 && t.psm != GS_PSM_T8 && t.psm != GS_PSM_T4) return {};
    if (t.psm != GS_PSM_CT32 && (t.cpsm != GS_PSM_CT32 || t.csm || t.csa ||
        e.texclut.cou || e.texclut.cov || e.texclut.cbw > 1)) return {};
    const auto textureRange = g144TextureRange(e);
    const auto paletteRange = g144ClutRange(e);
    if (!textureRange.valid) return {};
    const auto overlaps = [&](const G144BlockRange& surface) {
        return surface.valid && (g144RangeOverlaps(textureRange, surface) ||
               (t.psm != GS_PSM_CT32 && paletteRange.valid && g144RangeOverlaps(paletteRange, surface)));
    };
    // A materialized FBO is still dynamic even after its dirty flag is cleared.
    // Reject known producer ranges regardless of current CPU/GPU authority.
    for (uint32_t slot = 0; slot < kG248TargetCount; ++slot)
        if (overlaps(g_g261Res[slot].range)) return {};
    const auto& frame = e.ctx.frame;
    if (overlaps(g144RangeForRect(frame.fbp << 5u, frame.psm, frame.fbw,
                                 0, 0, uint32_t(frame.fbw)*64u, 512))) return {};
    // Preserve legacy replacement and diagnostic side effects when those systems
    // are requested. Compiled replacements have their own explicit variant switch.
#if defined(PS2X_G656_TEXREPL)
    auto& replacements = dc2_tex::TextureReplacementManager::instance();
    replacements.init();
    if (replacements.isEnabled() || replacements.isDumpEnabled()) return {};
#endif
    const dc2::assetcache::Descriptor desc{uint32_t(width), uint32_t(height), t.tbw, t.psm};
    auto image = cache->find(desc, vram, vramSize, t.tbp0, t.cbp,
                             dc2::assetcache::replacementsEnabled());
    uint64_t bad = 0;
    if (image && dc2::assetcache::verifyEnabled())
    {
        // Always compare ORIGINAL before optional replacement. The diagnostic
        // oracle is deliberately separate from clean performance gates.
        auto original = cache->find(desc, vram, vramSize, t.tbp0, t.cbp, false);
        std::vector<uint32_t> reference(size_t(width) * height);
        g178DecodeWhole(self, gs, vram, e.ctx, e.texa, e.texclut, width, height, reference.data());
        for (size_t n = 0; n < reference.size(); ++n) bad += reference[n] != original.pixels[n];
    }
    dc2::assetcache::noteResult(bool(image), uint64_t(width) * height, image.replacement, bad);
    if (bad) return {};
    return image;
}
