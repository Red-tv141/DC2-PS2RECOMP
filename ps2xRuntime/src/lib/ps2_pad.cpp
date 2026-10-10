#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cmath>
#include <string>
#include <utility>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;
    constexpr int kNoGamepad = -1;
    constexpr int kMaxGamepads = 4;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    int firstAvailableGamepad()
    {
        for (int gamepad = 0; gamepad < kMaxGamepads; ++gamepad)
        {
            if (IsGamepadAvailable(gamepad))
                return gamepad;
        }

        return kNoGamepad;
    }
}

// ---- Stick processing (G792: PCSX2 DualShock 2 model) ----
// Shared by all three input paths: dc2_poll_host_pad, PSPadBackend::readState and
// g449PollConfiguredPad, through convertStick() below.
//
// DC2 applies its OWN dead zone to every stick read. GetLX/GetLY/GetRX/GetRY__8CGamePad
// (0x14AF80..0x14AFB0) zero any byte within 0x32 of 0x80 and rescale the rest linearly
// ((b - 0xB1) * 128 / 0x4E), so only bytes <= 0x4E or >= 0xB2 move anything; the player
// runs once sqrt(lx^2 + ly^2) >= 0.8 (CharaControl). The game is tuned for DualShock 2
// bytes and the host must hand it DualShock-2-shaped bytes, not pre-shaped ones.
//
// The G7 overhaul stacked a 15 % rescaled dead zone and a 2.2 power curve IN FRONT of the
// game's: a cardinal push moved nothing until ~75 % of stick travel, then went from zero
// to full speed over the last quarter, so walking speed could not be controlled. And with
// no axis scale, a full DIAGONAL on a round-gate pad gave (0xD9,0xD9) -> game (0.51,0.51)
// -> magnitude 0.72 < 0.8: the player could never run diagonally and moved at half speed
// per axis. The launcher path (G449) used the same curve and so had the same defect.
//
// This mirrors PadDualshock2::Set (pcsx2/SIO/Pad/PadDualshock2.cpp) at PCSX2's defaults:
//   * each half axis is multiplied by AxisScale (default 1.33, "130 % to 140 % is
//     recommended for recent controllers") and clamped PER AXIS -- the DS2's square-ish
//     range, so a full diagonal reaches ~0xF7 on both axes;
//   * Deadzone (default 0) is radial, tested on the SCALED position, and does NOT
//     rescale: inside it the stick reads centre, outside it is untouched. A host dead
//     zone therefore composes with the game's as a max(), never as a sum;
//   * no response curve.
// Bytes follow PCSX2's MERGE() with the rest value at 0x80 (this port's convention for
// centre, recordings and DC2_LSTICK scripts): +h -> 0x80 + h/2, -h -> 0x80 - (h+1)/2,
// h = u8(|v| * scale * 255). Game movement starts at ~30 % travel, full at ~75 %.
//
// Env: DC2_STICK_SCALE (1.33), DC2_STICK_DEADZONE (0.00), DC2_STICK_CURVE /
// DC2_LSTICK_CURVE / DC2_RSTICK_CURVE (1.0 = linear; > 1 softens the centre; applied to
// the radial magnitude before the scale). Rollbacks: DC2_STICK_G7=1 restores the G7
// inner/outer dead zone + 2.2 curve pipeline (its DC2_STICK_DEADZONE default 0.15 and
// DC2_STICK_OUTER apply there); DC2_STICK_LINEAR=1 the original raw linear conversion.

static float stickEnvFloat(const char *name, float def)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return def;
    char *end = nullptr;
    float f = std::strtof(v, &end);
    return (end != v) ? f : def;
}

static bool stickEnvFlag(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

enum class StickModel
{
    Pcsx2,   // default: PCSX2 DualShock 2 (axis scale, non-rescaling dead zone, linear)
    G7,      // rollback DC2_STICK_G7=1: inner/outer rescaled dead zone + power curve
    Linear   // rollback DC2_STICK_LINEAR=1: raw linear conversion
};

struct StickParams
{
    StickModel model;
    float deadzone;     // Pcsx2: radial on the scaled position, no rescale. G7: inner radius.
    float outerDead;    // G7 only: outer dead zone radius (deadzone..1)
    float axisScale;    // Pcsx2 only: per-half-axis multiplier (PCSX2 "AxisScale")
    float leftCurve;    // response curve exponent for left stick (1 = linear)
    float rightCurve;   // response curve exponent for right stick
    float trigThresh;   // analog trigger press threshold
};

static const StickParams &getStickParams()
{
    static const StickParams p = []() {
        StickParams s;
        s.model = stickEnvFlag("DC2_STICK_LINEAR") ? StickModel::Linear
                : stickEnvFlag("DC2_STICK_G7")     ? StickModel::G7
                                                   : StickModel::Pcsx2;
        const bool g7 = (s.model == StickModel::G7);

        s.deadzone  = stickEnvFloat("DC2_STICK_DEADZONE", g7 ? 0.15f : 0.0f);
        s.outerDead = stickEnvFloat("DC2_STICK_OUTER", 0.95f);
        s.axisScale = stickEnvFloat("DC2_STICK_SCALE", 1.33f);
        float baseCurve = stickEnvFloat("DC2_STICK_CURVE", g7 ? 2.2f : 1.0f);
        s.leftCurve  = stickEnvFloat("DC2_LSTICK_CURVE", baseCurve);
        s.rightCurve = stickEnvFloat("DC2_RSTICK_CURVE", baseCurve);
        s.trigThresh = stickEnvFloat("DC2_TRIGGER_THRESH", 0.0f);

        // Clamp to sane ranges (axis scale: PCSX2's own 0.01..2.00 setting range).
        if (s.deadzone < 0.f) s.deadzone = 0.f;
        if (s.deadzone > 0.9f) s.deadzone = 0.9f;
        if (s.outerDead <= s.deadzone) s.outerDead = s.deadzone + 0.05f;
        if (s.outerDead > 1.0f) s.outerDead = 1.0f;
        if (!(s.axisScale >= 0.01f)) s.axisScale = 0.01f;
        if (s.axisScale > 2.0f) s.axisScale = 2.0f;
        if (s.leftCurve < 0.1f) s.leftCurve = 0.1f;
        if (s.rightCurve < 0.1f) s.rightCurve = 0.1f;

        if (s.model == StickModel::Pcsx2)
            std::fprintf(stderr,
                "[G792:stick] model=pcsx2-ds2 scale=%.2f deadzone=%.2f lCurve=%.1f rCurve=%.1f trigThresh=%.2f\n",
                s.axisScale, s.deadzone, s.leftCurve, s.rightCurve, s.trigThresh);
        else if (g7)
            std::fprintf(stderr,
                "[G7:stick] deadzone=%.2f outer=%.2f lCurve=%.1f rCurve=%.1f trigThresh=%.2f\n",
                s.deadzone, s.outerDead, s.leftCurve, s.rightCurve, s.trigThresh);
        return s;
    }();
    return p;
}

// PCSX2 DualShock 2 conversion (see the block comment above). rawX/rawY are the host
// axes in -1..+1 (raylib: +Y is DOWN, which is also the DS2's 0xFF direction).
static std::pair<uint8_t, uint8_t> processStickPcsx2(float rawX, float rawY,
                                                     float deadzone, float axisScale,
                                                     float curve)
{
    // Optional response curve on the radial magnitude (identity at the default 1.0).
    if (curve != 1.0f)
    {
        const float mag = std::sqrt(rawX * rawX + rawY * rawY);
        if (mag > 1e-6f)
        {
            const float k = std::pow(mag > 1.0f ? 1.0f : mag, curve) / mag;
            rawX *= k;
            rawY *= k;
        }
    }

    // PCSX2: rawInputs[i] = u8(clamp(value * axisScale * 255, 0, 255)) for each half axis.
    auto half = [axisScale](float v) -> int {
        float s = std::fabs(v) * axisScale * 255.0f;
        if (!(s > 0.0f)) return 0;
        if (s > 255.0f) s = 255.0f;
        return static_cast<int>(s);
    };
    const int hx = half(rawX);
    const int hy = half(rawY);

    // PCSX2's atan2 test (|pos| <= cos/sin(theta) * dz on both axes) is the circle
    // r <= dz, evaluated on the scaled, clamped position.
    if (deadzone > 0.0f)
    {
        const float px = static_cast<float>(hx) / 255.0f;
        const float py = static_cast<float>(hy) / 255.0f;
        if (px * px + py * py <= deadzone * deadzone)
            return { kPadStickCenter, kPadStickCenter };
    }

    // PCSX2 MERGE(), rest at 0x80: both ends reach 0x00 / 0xFF.
    auto toByte = [](float v, int h) -> uint8_t {
        if (h == 0) return kPadStickCenter;
        return (v > 0.0f) ? static_cast<uint8_t>(0x80 + h / 2)
                          : static_cast<uint8_t>(0x80 - (h + 1) / 2);
    };
    return { toByte(rawX, hx), toByte(rawY, hy) };
}

// G7 rollback (DC2_STICK_G7=1): deadzone → rescale → power curve → byte.
// The scale factor is applied to the ORIGINAL per-axis values (not a normalised
// direction), so the controller's native shape is preserved.
// curve > 1 = less sensitive near centre (2.2 typical); curve = 1 = linear.
static std::pair<uint8_t, uint8_t> processStickG7(float rawX, float rawY,
                                                   float innerDead, float outerDead,
                                                   float curve)
{
    const float mag = std::sqrt(rawX * rawX + rawY * rawY);
    if (mag < 1e-6f)
        return { 0x80u, 0x80u };

    // Inner deadzone — output is exactly centre.
    if (mag < innerDead)
        return { 0x80u, 0x80u };

    // Compute a 0..1 scaling factor from the radial magnitude.
    // At mag >= outerDead: scale = 1 (raw passthrough, full per-axis range).
    // At mag = innerDead: scale = 0 (smooth entry, no jump discontinuity).
    float scale;
    if (mag >= outerDead) {
        scale = 1.0f;
    } else {
        float range = outerDead - innerDead;
        if (range < 0.01f) range = 0.01f;
        float t = (mag - innerDead) / range;  // 0..1
        scale = std::pow(t, curve);
    }

    // Apply scale to ORIGINAL axes — preserves square/circular shape.
    float outX = rawX * scale;
    float outY = rawY * scale;

    // Clamp each axis independently (handles pads that report > 1.0).
    if (outX > 1.0f) outX = 1.0f; else if (outX < -1.0f) outX = -1.0f;
    if (outY > 1.0f) outY = 1.0f; else if (outY < -1.0f) outY = -1.0f;

    // Convert to 0x80-centred byte.
    auto toByte = [](float v) -> uint8_t {
        int b = 128 + static_cast<int>(v * 127.0f);
        if (b < 0) b = 0; else if (b > 255) b = 255;
        return static_cast<uint8_t>(b);
    };
    return { toByte(outX), toByte(outY) };
}

// Legacy linear conversion (rollback path, matches the original G7 behaviour).
static std::pair<uint8_t, uint8_t> processStickLinear(float rawX, float rawY,
                                                       float deadzone)
{
    const float mag = std::sqrt(rawX * rawX + rawY * rawY);
    if (mag < deadzone) { rawX = 0.f; rawY = 0.f; }
    auto toByte = [](float v) -> uint8_t {
        int b = 128 + static_cast<int>(v * 127.0f);
        if (b < 0) b = 0; else if (b > 255) b = 255;
        return static_cast<uint8_t>(b);
    };
    return { toByte(rawX), toByte(rawY) };
}

// The one entry point for all three input paths. `dead` and `curve` are the caller's
// (the built-in env values, or the launcher config); `linearDead` is what the
// DC2_STICK_LINEAR rollback used at that call site.
static std::pair<uint8_t, uint8_t> convertStick(float rawX, float rawY, float dead,
                                                float curve, float linearDead)
{
    const StickParams &sp = getStickParams();
    switch (sp.model)
    {
    case StickModel::Linear:
        return processStickLinear(rawX, rawY, linearDead);
    case StickModel::G7:
        return processStickG7(rawX, rawY, dead, sp.outerDead, curve);
    default:
        return processStickPcsx2(rawX, rawY, dead, sp.axisScale, curve);
    }
}

// G449: launcher controller remapping (DC2_CONTROLLER_CONFIG). Included here because it
// needs the scePad bit constants and firstAvailableGamepad() above, and because raylib's
// input API is only reachable from this TU. Entirely inert when the flag is unset.
#include "ps2_pad_parts/g449_controller_config.inc"

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    const int gamepad = firstAvailableGamepad();
    const bool useGamepad = (gamepad != kNoGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        // DC2_STICK_LINEAR here was always the raw conversion with no dead zone.
        const auto &sp = getStickParams();
        auto l = convertStick(lx, ly, sp.deadzone, sp.leftCurve, 0.0f);
        auto r = convertStick(rx, ry, sp.deadzone, sp.rightCurve, 0.0f);
        data[6] = l.first;  data[7] = l.second;
        data[4] = r.first;  data[5] = r.second;
    }
    else
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    // PHASE F21 — synthetic press removed from backend. The pad_button_read_stub
    // in dc2_game_override.cpp owns the synthetic schedule and must be the ONLY
    // synth source. Reason: read_pad__FP10PAD_STATUSii (decomp 0x14a490, line
    // 64443-64453) RESETS its state machine when the button mask CHANGES between
    // calls. Two synthetic sources with mismatched masks would thrash the state
    // and never deliver a button into CGamePad+0x04.

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

// ---------------------------------------------------------------------------
// PHASE G7 — live host controller (XInput on Windows, via raylib's GLFW joystick
// backend) mapped to the scePad bit layout. Polled once per host present frame on
// the raylib/main thread (see g7_poll_live_pad in dc2_game_override.cpp) and
// published to a snapshot the guest pad read consumes. Free function (NOT a header
// method) so no header edit / mass rebuild is needed; declared `extern` at the call
// site. Marker: G7_XINPUT_LIVE.
//
// Output mask is ACTIVE-HIGH (bit set == pressed) in the same 16-bit scePad layout
// the override uses. Analog bytes are 0x80-centred, full 0x00..0xFF range with a
// radial deadzone. Returns true when a host device is providing input (a gamepad is
// connected, or keyboard fallback is allowed). Bit layout (scePad):
//   Select 0x0001 L3 0x0002 R3 0x0004 Start 0x0008 Up 0x0010 Right 0x0020
//   Down 0x0040 Left 0x0080 L2 0x0100 R2 0x0200 L1 0x0400 R1 0x0800
//   Triangle 0x1000 Circle 0x2000 Cross 0x4000 Square 0x8000
extern "C" bool dc2_poll_host_pad(bool allowKeyboard, uint16_t *outMask,
                                  uint8_t *outLX, uint8_t *outLY,
                                  uint8_t *outRX, uint8_t *outRY)
{
    // G449: a launcher controller config replaces the built-in mapping wholesale. It is
    // consulted FIRST and before the "no pad and no keyboard" early-out below, because a
    // config can bind every control to the keyboard — in which case there is live input to
    // report even with no gamepad attached, and the built-in path would have refused it.
    if (g449PollConfiguredPad(outMask, outLX, outLY, outRX, outRY))
        return true;

    const int gamepad = firstAvailableGamepad();
    const bool padConnected = (gamepad != kNoGamepad);
    if (!padConnected && !allowKeyboard)
        return false;

    uint16_t mask = 0u; // active-high
    auto press = [&mask](uint16_t bit) { mask |= bit; };

    // Stick processing uses the shared pipeline (convertStick).
    const auto &sp = getStickParams();

    uint8_t lx = kPadStickCenter, ly = kPadStickCenter;
    uint8_t rx = kPadStickCenter, ry = kPadStickCenter;

    if (padConnected)
    {
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))    press(PAD_UP);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))  press(PAD_DOWN);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  press(PAD_LEFT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) press(PAD_RIGHT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) press(PAD_CROSS);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))press(PAD_CIRCLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) press(PAD_SQUARE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))   press(PAD_TRIANGLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))  press(PAD_L1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) press(PAD_R1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))  press(PAD_L2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2)) press(PAD_R2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))    press(PAD_START);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))     press(PAD_SELECT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB))      press(PAD_L3);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB))     press(PAD_R3);

        // Analog triggers also fire L2/R2 (digital pull) for games that read them.
        // Threshold is configurable (DC2_TRIGGER_THRESH, default 0.0 = 50% pull on
        // XInput's -1..+1 range). Old value was -0.5 (25% pull, too sensitive).
        if (GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_TRIGGER)  > sp.trigThresh) press(PAD_L2);
        if (GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > sp.trigThresh) press(PAD_R2);

        // Sticks: PCSX2 DS2 model by default; rollback flags select the older pipelines.
        const float rawLX = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        const float rawLY = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        const float rawRX = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        const float rawRY = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        auto l = convertStick(rawLX, rawLY, sp.deadzone, sp.leftCurve, 0.20f);
        auto r = convertStick(rawRX, rawRY, sp.deadzone, sp.rightCurve, 0.20f);
        lx = l.first; ly = l.second; rx = r.first; ry = r.second;
    }
    else // keyboard fallback (opt-in)
    {
        if (IsKeyDown(KEY_UP))    press(PAD_UP);
        if (IsKeyDown(KEY_DOWN))  press(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT))  press(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT)) press(PAD_RIGHT);
        if (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_SPACE)) press(PAD_CROSS);
        if (IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_BACKSPACE)) press(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z)) press(PAD_SQUARE);
        if (IsKeyDown(KEY_X)) press(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q)) press(PAD_L1);
        if (IsKeyDown(KEY_E)) press(PAD_R1);
        if (IsKeyDown(KEY_ONE)) press(PAD_L2);
        if (IsKeyDown(KEY_THREE)) press(PAD_R2);
        if (IsKeyDown(KEY_ENTER)) press(PAD_START);
        if (IsKeyDown(KEY_TAB)) press(PAD_SELECT);
        // Left stick on WASD so free-roam movement works on keyboard.
        // Keyboard is binary (no curve); full-scale deflection when held -- on the
        // diagonals too, as a DS2 (and PCSX2's keyboard binding) reports it.
        float kx = (IsKeyDown(KEY_D) ? 1.f : 0.f) - (IsKeyDown(KEY_A) ? 1.f : 0.f);
        float ky = (IsKeyDown(KEY_S) ? 1.f : 0.f) - (IsKeyDown(KEY_W) ? 1.f : 0.f);
        auto toByte = [](float v) -> uint8_t {
            return (v > 0.f) ? 0xFFu : (v < 0.f) ? 0x00u : kPadStickCenter;
        };
        lx = toByte(kx); ly = toByte(ky);
    }

    if (outMask) *outMask = mask;
    if (outLX) *outLX = lx;
    if (outLY) *outLY = ly;
    if (outRX) *outRX = rx;
    if (outRY) *outRY = ry;
    return true;
}
