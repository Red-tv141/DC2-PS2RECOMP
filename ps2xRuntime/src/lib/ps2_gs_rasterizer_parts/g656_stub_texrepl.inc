// ===== G656 P17: constant-folding stub for the HD texture replacement & dumping subsystem =====
//
// WHY. `tools/g654_tu_price.py` plus a per-COMDAT rank over the shipped G655
// `ps2_gs_rasterizer.obj` (see plans/g656-p1-hot-tu-residual.md) found this subsystem to be the
// single largest run-time-inactive block still compiled into the hottest TU in the build:
//
//     dc2_tex   45,753 bytes / 590 COMDATs   (the manager, its maps, its string/vector churn)
//     stb_image 29,232 bytes /  76 COMDATs   (PNG decode + encode, reached only from it)
//     xxHash    21,515 bytes /  42 COMDATs   (texture keys; NO other user in this TU)
//     ------------------------------------
//     96,500 bytes = 8.19% of ps2_gs_rasterizer.cpp's machine code
//
// It is OFF in every normal Release run: `TextureReplacementManager::init()` resolves `m_enabled`
// from `DC2_TEXTURE_REPLACEMENTS` and `m_dumpEnabled` from `DC2_TEXTURE_DUMP`, and
// `tools/run_g477_perf.ps1` clears both on every measured run. So by rule 12c its 96,500 bytes buy
// a code-layout tax and nothing else, and by rule 16 the fix is to compile them OUT rather than
// leave them merely off. The two precedents both converted at this exact magnitude: G654 P5 removed
// 9.47% for -1.082 ms/f and the G655 aggregate removed 8.67% for -0.943 ms/f.
//
// ⚠️ THIS REMOVES A FEATURE FROM THE DEFAULT BUILD, not just a diagnostic. That is deliberate and
// it is reversible by exactly one CMake option: `-DPS2X_G656_TEXREPL=ON` restores the real
// implementation byte for byte, and `DC2_TEXTURE_REPLACEMENTS=1` / `DC2_TEXTURE_DUMP=1` behave
// there exactly as they do today. Nothing about the DEFAULT configuration's rendering changes,
// because in the default configuration the subsystem never did anything.
//
// EXACTNESS OF THE STUB. There are exactly two call sites, both in
// rasterizer_vram_materialization.inc's texture-decode loop:
//
//   * `init()`               — with both env flags unset, `checkEnabled()` is false, the
//                              `if (m_enabled)` scan is skipped, and the call has no effect
//                              outside the manager's own state. The stub is empty.
//   * `tryReplaceTexture(..., inOutW, inOutH, inOutPx)`
//                            — ps2_texture_replacements.inc:351 returns false at
//                              `if (!m_enabled && !m_dumpEnabled && !g_g620LiveTexRepl)`, and that
//                              early return is documented there as leaving `inOutW`, `inOutH` and
//                              `inOutPx` untouched. The stub returns false and touches nothing.
//                              Its result is discarded at the call site in either build.
//
// The only observable difference is the `m_stats.texturesEncountered` counter, which is read by
// nothing outside a `-DPS2X_G656_TEXREPL=ON` build's own report.

#pragma once

namespace dc2_tex
{

struct TextureReplacementManager
{
    static TextureReplacementManager &instance()
    {
        static TextureReplacementManager s;
        return s;
    }

    void init() {}

    bool isEnabled() const { return false; }

    bool tryReplaceTexture(uint8_t * /*vram*/, size_t /*vramSize*/,
                           const GSContext & /*ctx*/,
                           const GSTexaReg & /*texa*/,
                           const GSTexClutReg & /*texclut*/,
                           bool /*isFboSrc*/, bool /*isViewKey*/,
                           int & /*inOutW*/, int & /*inOutH*/,
                           std::vector<uint32_t> & /*inOutPx*/)
    {
        return false;
    }
};

} // namespace dc2_tex
