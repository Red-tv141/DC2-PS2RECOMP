// ============================================================================================
// G675 — THE PS2-LOGICAL <-> HOST-PHYSICAL RESOLUTION MAPPING. ONE DEFINITION, FOUR TUs.
// ============================================================================================
//
// Included by ps2_gs_gpu.cpp, ps2_gs_gpu_raster.cpp, ps2_gs_rasterizer.cpp and ps2_runtime.cpp
// (via their `*_parts/*.inc` headers). Before G675 this block was COPY-PASTED into all four and
// had already drifted: the rasterizer copy carried only the scale accessors.
//
// ⭐⭐⭐ THE LAW THIS FILE EXISTS TO ENFORCE
//
//     physical = logical * kScale,   kScale a positive INTEGER,   for EVERY GS resource,
//     with NO per-target special case and NO separate X/Y factor.
//
// Everything the GS front-end sees — FBP, FBW, PSM, TBP, ZBUF, SCISSOR, XYOFFSET, VRAM
// addressing, the 12.4 vertex grid, readback contents — stays in ORIGINAL PS2 LOGICAL SPACE.
// The scale is applied at exactly one boundary: where a logical GS resource is mapped to its
// physical GPU representation (FBO/depth allocation, viewport, scissor, and the resident-view
// samplers that convert a logical GS pixel coordinate into a physical texel).
//
// ⛔ WHY AN INTEGER, AND WHY ONE FACTOR — this is the bug G675 repaired, not a style choice.
//
// The pre-G675 derivation was `scaleX = reqW/640`, `scaleY = reqH/448`, plus a SNAP: a logical
// width of 512 or 640 became `reqW` and a logical height of 415/416/448 became `reqH`. That
// produced THREE independent ways for a logical coordinate to meet the wrong physical ratio:
//
//   1. A 512-wide target got physW = reqW, so its true ratio was reqW/512, while the shader was
//      handed uFbScale.x = reqW/640 — every G309 / G281 / G338 resident-view fetch on that
//      surface was off by 640/512 = 1.25x. DC2's DISPLAY target is 512 logical wide
//      (`readDisplayFboOnWorker` probes `g349FboKey(fbp, 512)` first), so this was not a corner.
//   2. A 416-tall target got physH = reqH (ratio reqH/416) against uFbScale.y = reqH/448 — a 7.7%
//      vertical drift, i.e. a band that grows down the surface.
//   3. scaleX != scaleY (1920/640 = 3.000 vs 1080/448 = 2.411) anisotropically stretched the
//      4:3 raster to fill a 16:9 window — the "FOV stretch" known quirk. That is a PRESENTATION
//      decision being smuggled into the RASTER stage.
//
// With one integer S, `physW/w == physH/h == S` for every entry by construction, so a single
// uFbScale uniform is exact for every target, `round()` never lands off-grid, a logical pixel is
// always an exact SxS block of physical pixels (so the readback decimation has a well-defined
// centre tap), and the 1x path is bit-for-bit the identity.
//
// PRESENTATION IS SEPARATE AND STAYS SEPARATE. `DC2_RESOLUTION_WIDTH/HEIGHT` size the HOST
// WINDOW. The GS raster runs at 640S x 448S in PS2 aspect; the presentation blit aspect-fits that
// already-high-resolution image into the window (pillarbox/letterbox). Presentation never creates
// the resolution increase and never stretches the raster.
//
// ============================================================================================

// G675: the derived scale below depends on the aspect the finished frame is PRESENTED at, which
// the field-of-view override changes. Separate concern, separate file; included here because the
// derivation genuinely needs it (and it is include-guarded, so a TU may include both).
#include "ps2_widescreen.inc"

struct DC2NativeRenderConfig
{
    bool isConfigured = false;
    bool isNativeHighRes = false;   // scale > 1
    bool isFullscreen = false;
    int requestedWidth = 0;         // HOST WINDOW width  (presentation only)
    int requestedHeight = 0;        // HOST WINDOW height (presentation only)
    int scale = 1;                  // ⭐ the one integer that maps logical -> physical
    float scaleX = 1.0f;            // == scale, kept as float for the shader uniform
    float scaleY = 1.0f;            // == scale, ALWAYS equal to scaleX
};

// The PS2 logical display geometry this port rasterizes into. Physical display storage is
// exactly kDc2LogicalDisplayWidth * scale by kDc2LogicalDisplayHeight * scale.
inline constexpr int kDc2LogicalDisplayWidth = 640;
inline constexpr int kDc2LogicalDisplayHeight = 448;
inline constexpr int kDc2MaxNativeScale = 8;

inline DC2NativeRenderConfig &getDC2NativeRenderConfig()
{
    static DC2NativeRenderConfig s_config = []()
    {
        DC2NativeRenderConfig cfg{};
        const char *wEnv = std::getenv("DC2_RESOLUTION_WIDTH");
        const char *hEnv = std::getenv("DC2_RESOLUTION_HEIGHT");
        const char *fsEnv = std::getenv("DC2_FULLSCREEN");
        const char *sEnv = std::getenv("DC2_INTERNAL_SCALE");

        int w = 0, h = 0;
        if (wEnv && wEnv[0] != '\0')
            w = std::atoi(wEnv);
        if (hEnv && hEnv[0] != '\0')
            h = std::atoi(hEnv);

        cfg.isFullscreen = (fsEnv && fsEnv[0] != '\0' && std::strcmp(fsEnv, "0") != 0);
        cfg.requestedWidth = (w > 0) ? w : kDc2LogicalDisplayWidth;
        cfg.requestedHeight = (h > 0) ? h : kDc2LogicalDisplayHeight;

        // DC2_INTERNAL_SCALE is the explicit form and wins. It is also what makes a scale A/B
        // legitimate: the SAME window size at S=1 and S=N, so a pixel comparison isolates the
        // raster stage from the presentation blit.
        int scale = 0;
        if (sEnv && sEnv[0] != '\0')
            scale = std::atoi(sEnv);

        if (scale <= 0)
        {
            // Derived form: the smallest integer S whose physical raster is at least as dense as
            // the PRESENTED image. The presented image is the window rectangle after aspect-fit
            // to whatever aspect the frame is shown at, so the width that actually reaches the
            // screen is min(reqW, reqH * presentedAspect) — never the raw window width on a
            // display wider than the rendered frame.
            //
            // presentedAspect is 640:448 by default and the DC2_ASPECT override when armed: with
            // the field-of-view expansion the same 640-column raster carries a 16:9 view, so a
            // 1920-wide window then genuinely needs S=3 across its full width.
            const double presentedAspect =
                dc2AspectOverrideActive()
                    ? static_cast<double>(dc2GetDisplayAspect())
                    : (static_cast<double>(kDc2LogicalDisplayWidth) /
                       static_cast<double>(kDc2LogicalDisplayHeight));
            const double byWidth = static_cast<double>(cfg.requestedWidth);
            const double byHeight = static_cast<double>(cfg.requestedHeight) * presentedAspect;
            const double fitW = (byWidth < byHeight) ? byWidth : byHeight;
            // Integer ceiling without <cmath>; the 1e-3 slack keeps an exact multiple on its step
            // (1280x720 -> 2, not 3) and 640x448 -> 1 exactly.
            scale = 1;
            while (scale < kDc2MaxNativeScale &&
                   static_cast<double>(scale) * kDc2LogicalDisplayWidth + 1e-3 < fitW)
                ++scale;
        }

        if (scale < 1)
            scale = 1;
        if (scale > kDc2MaxNativeScale)
            scale = kDc2MaxNativeScale;

        cfg.scale = scale;
        cfg.scaleX = static_cast<float>(scale);
        cfg.scaleY = static_cast<float>(scale);
        cfg.isNativeHighRes = (scale > 1);
        cfg.isConfigured = true;
        return cfg;
    }();
    return s_config;
}

// ⭐ The scale itself. Every conversion below is this one multiply.
inline int dc2GetNativeScale()
{
    return getDC2NativeRenderConfig().scale;
}

inline bool dc2IsNativeHighResActive()
{
    return getDC2NativeRenderConfig().isNativeHighRes;
}

inline float dc2GetNativeScaleX()
{
    return getDC2NativeRenderConfig().scaleX;
}

inline float dc2GetNativeScaleY()
{
    return getDC2NativeRenderConfig().scaleY;
}

// LOGICAL -> PHYSICAL. No snapping, no rounding, no per-target family. `logical <= 0` is passed
// through unchanged so a caller probing an unallocated entry keeps its sentinel.
inline int dc2GetNativePhysicalWidth(int logicalW)
{
    if (logicalW <= 0)
        return logicalW;
    return logicalW * dc2GetNativeScale();
}

inline int dc2GetNativePhysicalHeight(int logicalH)
{
    if (logicalH <= 0)
        return logicalH;
    return logicalH * dc2GetNativeScale();
}

// The PCRTC/display conversion target. Distinct from GS raster resolution by construction: this
// is the PS2 display geometry taken to physical density, NOT the host window size.
inline int dc2GetNativePhysicalDisplayWidth()
{
    return kDc2LogicalDisplayWidth * dc2GetNativeScale();
}

inline int dc2GetNativePhysicalDisplayHeight()
{
    return kDc2LogicalDisplayHeight * dc2GetNativeScale();
}

inline int dc2GetNativePhysicalDisplayStorageHeight()
{
    return kDc2LogicalDisplayHeight * dc2GetNativeScale();
}

// ⭐ The centre tap of the SxS physical block that backs one logical pixel. Exactly 0 at S=1, so
// every decimation site below is the identity on the 1x reference path.
inline int dc2NativeSubPixelCentre()
{
    return dc2GetNativeScale() / 2;
}

// Diagnostic only: DC2_NATIVE_RES_DIAG=1 arms the resolution-pipeline trace and the per-surface
// PPM dumps in the GL backend. Never on in a shipped or timed run.
inline bool dc2NativeResDiagOn()
{
    static const bool s_diag = (std::getenv("DC2_NATIVE_RES_DIAG") != nullptr);
    return s_diag;
}
