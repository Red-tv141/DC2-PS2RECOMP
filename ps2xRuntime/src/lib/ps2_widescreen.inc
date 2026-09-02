#ifndef PS2X_G675_WIDESCREEN_INC
#define PS2X_G675_WIDESCREEN_INC
#include <atomic>
#include <cstdlib>
#include <cstring>
// ============================================================================================
// G675 — DISPLAY ASPECT AND TRUE FIELD-OF-VIEW EXPANSION (Hor+). ONE DEFINITION, THREE TUs.
// ============================================================================================
//
// ⛔ THIS IS NOT ANAMORPHIC WIDESCREEN. Nothing here stretches a 4:3 image. It changes the
// GUEST'S OWN PROJECTION so the game renders MORE OF THE WORLD horizontally, and then tells the
// presentation blit that the result is meant to be shown at that wider aspect. The vertical
// field of view is untouched — the standard "Hor+" convention.
//
// ⭐ WHERE THE KNOB IS, AND WHY IT IS THE RIGHT ONE (static analysis, ref/functions)
//
//   mgSetProjection__Ff @0x00143620
//     -> mgSetRenderInfo__Ffff @0x00143600      (passes mgScreenWidth/Height/ZDepth)
//        -> SetRenderInfo__13mgRENDER_INFOFfiiffi @0x00138B00, on mgRenderInfo @0x00380EC0
//
// SetRenderInfo builds the frame's transform as  view * PERSPECTIVE * ASPECT * SCREEN, where
//
//   PERSPECTIVE  m00 = proj / (0.55 * W)      m11 = proj / (0.55 * H)      (proj = the
//                                                                          mgSetProjection arg)
//   ASPECT       m00 = 1.0f                   m11 = 1792 / (3 * W)         (mgRenderInfo + 0xD0)
//   SCREEN       m00 = 0.55 * W               m11 = 0.55 * H,  + 2048 offset
//
// The 0.55*W terms cancel between PERSPECTIVE and SCREEN, so the effective horizontal focal
// length in GS units is exactly `proj * ASPECT.m00`, and ASPECT.m11 = 1792/(3W) = (H/W)*(4/3) —
// i.e. **the game's projection has 4:3 baked into it as a literal**. ASPECT.m00 is a hardcoded
// 1.0f (`lui v0,0x3f80 ; sw v0,0xd0(s0)` at 0x00138D78) that exists solely to make that matrix a
// unit matrix in x. It is therefore the one term that scales the horizontal FOV and nothing else:
//
//                    ASPECT.m00 = (4/3) / targetAspect
//
// 16:9 -> 0.75, i.e. a world point that used to land on the screen edge now lands at 75% of the
// way out, and a third more world becomes visible on each side. The vertical column of the matrix
// is untouched, so vertical FOV is bit-identical to the 4:3 build.
//
// ⭐ WHY IT CAN BE DONE WITHOUT REGENERATING ANYTHING. This is a static recompilation: patching
// the guest INSTRUCTION would do nothing, because the recompiled C++ is what executes. But
// SetRenderInfo flushes that matrix through `sceVu0CopyMatrix@0x00107180`, which
// `config_dc2_final.toml` lists as a STUB — so the call lands in ps2_stubs, in this runtime, with
// the matrix already written and not yet consumed. Scaling m00 there is exactly equivalent to the
// game having computed it, and every downstream consumer (the composed projections at
// mgRenderInfo+0x1E0 / +0x2A0, the frustum culling, the VU1 upload, the screen mapping) sees one
// consistent transform. See Kernel/Stubs/VU.cpp.
//
// ⚠️ KNOWN AND INTENDED CONSEQUENCES (they are properties of Hor+, not defects):
//   * 2D/UI (HUD, menus, dialogue, FMV) is authored in SCREEN space and does not go through this
//     projection, so it keeps its 4:3 layout and is stretched by the wider presentation. That is
//     what every PS2 widescreen hack does; the alternative is re-laying-out the game's 2D.
//   * Content the artists never expected to be on screen becomes visible at the left/right edges,
//     and DC2's own per-object culling may pop it in later than an artist would have liked.
//
// ============================================================================================

struct DC2AspectConfig
{
    bool configured = false;
    bool enabled = false;     // an explicit aspect was requested
    float displayAspect = 0.0f;  // e.g. 1.777778 for 16:9; 0 = "present as-is" (default)
    float fovScaleX = 1.0f;      // ASPECT.m00 replacement; exactly 1.0f when disabled
};

// The display aspect the guest's own projection is built for. Derived, not assumed: SetRenderInfo
// computes the vertical aspect term as 1792/(3*W), and with the shipped W=512 / H=448 that is
// (H/W) * 4/3 — the 4/3 is the literal 1792 = 448*4 against the 3*W denominator.
inline constexpr float kDc2NativeDisplayAspect = 4.0f / 3.0f;

inline DC2AspectConfig &getDC2AspectConfig()
{
    static DC2AspectConfig s_cfg = []()
    {
        DC2AspectConfig cfg{};
        cfg.configured = true;

        // DC2_ASPECT accepts "auto", "16:9", "16/9", or a decimal ("1.7778"). DC2_WIDESCREEN=1 is
        // the shorthand for 16:9. Unset => nothing changes anywhere.
        //
        // ⭐ "auto" = the requested WINDOW aspect (DC2_RESOLUTION_WIDTH / _HEIGHT), so any output
        // shape is covered without naming it — 1600x720 gives 2.2222 and fills the window instead
        // of pillarboxing a forced 16:9 frame inside it.
        //
        // ⛔ It is deliberately derived from the REQUESTED size and not from the live window, and
        // that is a correctness requirement, not a shortcut: this header is compiled into several
        // translation units (the GL backend reads `dc2GetFovScaleX()` to scale screen-space
        // sprites; the projection hook in Kernel/Stubs/VU.cpp reads it to widen the frustum), and
        // every one of them must resolve to the SAME number or the HUD and the world would be
        // scaled by different factors. An env-derived value is identical in every TU by
        // construction; a live, mutable one would not be. Consequence: dragging the window to a
        // new shape does not re-derive the field of view — relaunch to change it.
        const char *env = std::getenv("DC2_ASPECT");
        float aspect = 0.0f;
        if (env != nullptr && env[0] != '\0')
        {
            if (std::strcmp(env, "auto") == 0 || std::strcmp(env, "AUTO") == 0 ||
                std::strcmp(env, "Auto") == 0)
            {
                // Parsed here rather than read from DC2NativeRenderConfig: this header is included
                // on its own by Kernel/Stubs/VU.cpp, and is itself included BY ps2_native_res.inc,
                // so it must not depend on that struct existing.
                const char *wEnv = std::getenv("DC2_RESOLUTION_WIDTH");
                const char *hEnv = std::getenv("DC2_RESOLUTION_HEIGHT");
                const int w = (wEnv && wEnv[0] != '\0') ? std::atoi(wEnv) : 0;
                const int h = (hEnv && hEnv[0] != '\0') ? std::atoi(hEnv) : 0;
                if (w > 0 && h > 0)
                    aspect = static_cast<float>(w) / static_cast<float>(h);
            }
            else
            {
                const char *sep = std::strpbrk(env, ":/");
                if (sep != nullptr)
                {
                    const double num = std::atof(env);
                    const double den = std::atof(sep + 1);
                    if (num > 0.0 && den > 0.0)
                        aspect = static_cast<float>(num / den);
                }
                else
                {
                    aspect = static_cast<float>(std::atof(env));
                }
            }
        }
        else
        {
            const char *ws = std::getenv("DC2_WIDESCREEN");
            if (ws != nullptr && ws[0] != '\0' && std::strcmp(ws, "0") != 0)
                aspect = 16.0f / 9.0f;
        }

        // Refuse nonsense rather than silently rendering a degenerate frustum.
        if (aspect > 0.5f && aspect < 5.0f)
        {
            cfg.enabled = true;
            cfg.displayAspect = aspect;
            cfg.fovScaleX = kDc2NativeDisplayAspect / aspect;
        }
        return cfg;
    }();
    return s_cfg;
}

inline bool dc2AspectOverrideActive()
{
    return getDC2AspectConfig().enabled;
}

// The aspect the finished frame is meant to be DISPLAYED at. 0 when unset — callers then keep
// their existing behaviour (present the source rectangle at its own pixel aspect), so the default
// build is untouched.
inline float dc2GetDisplayAspect()
{
    return getDC2AspectConfig().displayAspect;
}

// The replacement for mgRENDER_INFO's ASPECT.m00. Exactly 1.0f when no override is armed, so the
// hook below is the identity on a default run.
inline float dc2GetFovScaleX()
{
    return getDC2AspectConfig().fovScaleX;
}

// ⭐ G675: how a SCREEN-SPACE 2D quad that already spans the whole scissor behaves under a widened
// field of view.
//
//   0 — NONE (**default**). Screen-space 2D is left exactly where the game put it, so it stretches
//       with the frame like every other PS2 widescreen hack. ARTEFACT-FREE BY CONSTRUCTION.
//   1 — UNIFORM. Every screen-space sprite is scaled about the centre by fovScaleX, so the HUD
//       keeps its 4:3 proportions — at the cost of the world-aligned overlays below.
//   2 — FULL-WIDTH PASS-THROUGH. As `uniform`, except a sprite already covering (nearly) the whole
//       logical width is left unscaled so it still covers the frame.
//
// ⚠️⚠️ `none` IS THE DEFAULT BECAUSE 2D SCALING IS THREE DECISIONS, NOT ONE, AND THE PER-TEXTURE
// ASSIGNMENT IS UNSOLVED (G675 §6.11). HUD furniture needs `uniform`; full-screen fades / letterbox
// bars / backdrops need `fullwidth`; world-aligned overlays need `none`. Every global setting is
// therefore wrong somewhere, and only `none` is wrong in a way that is merely COSMETIC (a stretched
// HUD) rather than CORRUPTING (misaligned, colour-destroyed overlays). Ship the cosmetic failure.
//
// The curated-identity route that would fix this properly is built but inert: `Mods/2d_noscale.txt`
// keyed by decoded-image content hash (ps2_g676_noscale.inc). ⛔ It cannot be filled from asset
// NAMES — the game composes its own texture pages at upload time, so the ISO's images match runtime
// textures 2 of 2,141 whole and 0 of 12 as sub-rectangles. FMV is handled structurally, not by the
// list (see below and G675 §6.10).
//
// ⛔⛔ WHY THE LIST HAS TO EXIST — MEASURED, NOT CAUTIOUS. DC2 draws its dungeon caustics as
// a SCREEN-COVERING GRID OF SCREEN-SPACE SPRITES: 5x5 tiles of 128x128, PSMT8 (paletted), tbp
// 0x2f20, spanning (-96,-96)..(384,384) on the DISPLAY target — structurally indistinguishable from
// a HUD widget per draw (same TME+FST, same z=0/zw=0/blend=1, and each tile is far too narrow for a
// full-width test to catch). Scaling that set by 0.75 MINIFIES a paletted pattern, and undersampling
// a CLUT picks unrelated palette entries: the dungeon floor turns into rainbow blocks. Measured on
// `dungeon1` (neighbour-chroma-jump on the ground band, 4:3 reference = 2.83):
//     16:9 + uniform, S=1 : 6.25   (broken)      16:9 + uniform, S=2 : 3.80  (mostly recovered)
//     16:9 + none         : 2.83   (identical to the 4:3 reference at every key)
// The FOV widening itself is innocent — an arm with the projection widened and 2D scaling off is
// pixel-clean. Distinguishing "world-aligned overlay" from "HUD" needs a set-level property that
// no per-draw signal in G178Draw carries, so the honest default is not to scale.
//
// Selected with DC2_2D_SCALE=none|uniform|fullwidth (aliases 0/1/2). Inert unless an aspect
// override is armed.
inline int dc2Get2dScaleMode()
{
    static const int s_mode = []() {
        const char *v = std::getenv("DC2_2D_SCALE");
        if (v == nullptr || v[0] == '\0')
            return 0; // ⭐ default: NONE — see the note above; `uniform` breaks world-aligned 2D
        if (std::strcmp(v, "none") == 0 || std::strcmp(v, "NONE") == 0 ||
            std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0)
            return 0;
        if (std::strcmp(v, "fullwidth") == 0 || std::strcmp(v, "FULLWIDTH") == 0 ||
            std::strcmp(v, "full") == 0 || std::strcmp(v, "2") == 0)
            return 2;
        if (std::strcmp(v, "uniform") == 0 || std::strcmp(v, "UNIFORM") == 0 ||
            std::strcmp(v, "1") == 0)
            return 1;
        // `list` is `uniform` restricted to the allow-list; the restriction itself lives in
        // ps2_g676_noscale.inc, which is where the keys are.
        if (std::strcmp(v, "list") == 0 || std::strcmp(v, "LIST") == 0)
            return 1;
        return 1; // unrecognised value: fall back to the default rather than silently disabling
    }();
    return s_mode;
}

inline bool dc2FullWidth2dOn()
{
    return dc2Get2dScaleMode() == 2;
}

// Is screen-space 2D scaled at all? False in the default `none` mode.
inline bool dc2Scale2dOn()
{
    return dc2Get2dScaleMode() != 0;
}

// ============================================================================================
// ⭐ G675 — FMV IS ALWAYS 4:3, AND IS NEVER SCALED.
// ============================================================================================
//
// An FMV is pre-rendered video: there is no projection to widen and no geometry to reveal, so the
// only thing a wider aspect can do to it is STRETCH it. The movie must therefore be presented at
// its own 4:3 aspect (pillarboxed inside a wider window) regardless of `DC2_ASPECT`.
//
// The signal is a monotonic COUNTER bumped by the MPEG decode stub each time it serves a picture
// (`sceMpegGetPicture`, Kernel/Stubs/MPEG.cpp), plus the tick at which it last moved. Presentation
// treats "the counter moved within the last N present ticks" as "a movie is on screen". A counter
// rather than a bool because the decoder has no single reliable "stopped" event — a stream can end,
// fail, be reset, or simply stop being asked for pictures — and a stale `true` would pillarbox the
// whole game. Decay makes the failure mode self-correcting in every one of those cases.
//
// ⛔ Read/written across threads (MPEG decode thread vs the present thread), hence relaxed atomics;
// exactness does not matter, only that the value eventually moves.
inline std::atomic<uint64_t> &dc2FmvPictureCounter()
{
    static std::atomic<uint64_t> s_counter{0};
    return s_counter;
}

inline void dc2NoteFmvPicture()
{
    dc2FmvPictureCounter().fetch_add(1u, std::memory_order_relaxed);
}

// DC2_FMV_KEEP_ASPECT=0 opts OUT (lets a movie stretch with the frame, the pre-G675 behaviour).
inline bool dc2FmvForce43On()
{
    static const bool s_on = []() {
        const char *v = std::getenv("DC2_FMV_KEEP_ASPECT");
        return !(v != nullptr && v[0] == '0' && v[1] == '\0');
    }();
    return s_on;
}

// Called once per present tick. Returns true while a movie is on screen.
inline bool dc2FmvActiveTick()
{
    static uint64_t s_lastCounter = 0;
    static int s_idleTicks = 1 << 20;
    const uint64_t now = dc2FmvPictureCounter().load(std::memory_order_relaxed);
    if (now != s_lastCounter)
    {
        s_lastCounter = now;
        s_idleTicks = 0;
    }
    else if (s_idleTicks < (1 << 20))
    {
        ++s_idleTicks;
    }
    // ~8 present ticks of grace: an FMV serves a picture every other tick at worst (video is
    // 30/25 fps against a 60 Hz present), and the last frame of a movie lingers briefly.
    return s_idleTicks < 8;
}

inline constexpr float kDc2FmvAspect = 4.0f / 3.0f;

// ⭐ G675 DIAGNOSTIC SPLIT — `DC2_ASPECT` does three separable things, and when one of them causes
// an artefact you have to be able to switch them off one at a time:
//   1. widen the guest's PROJECTION           -> DC2_G675_NO_FOV=1 disables
//   2. scale screen-space SPRITES             -> DC2_G675_NO_2DSCALE=1 disables
//   3. present at the requested display aspect (always on when DC2_ASPECT is set)
// Default: both on. These are diagnostics, not shipping levers — with (1) off the world is not
// actually widened, so the frame is anamorphic.
inline bool dc2FovProjectionOn()
{
    static const bool s_off = (std::getenv("DC2_G675_NO_FOV") != nullptr);
    return !s_off;
}

inline bool dc2Sprite2dScaleOn()
{
    static const bool s_off = (std::getenv("DC2_G675_NO_2DSCALE") != nullptr);
    return !s_off;
}

// The X scale actually applied to screen-space sprites (respects the diagnostic split).
inline float dc2GetSpriteScaleX()
{
    return (dc2Sprite2dScaleOn() && dc2Scale2dOn()) ? dc2GetFovScaleX() : 1.0f;
}

// The X scale actually applied to the guest projection (respects the diagnostic split).
inline float dc2GetProjectionScaleX()
{
    return dc2FovProjectionOn() ? dc2GetFovScaleX() : 1.0f;
}

#endif // PS2X_G675_WIDESCREEN_INC
