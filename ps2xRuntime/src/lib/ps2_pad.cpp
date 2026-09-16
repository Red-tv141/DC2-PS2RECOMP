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

// ---- Stick processing pipeline (G7 stick overhaul) ----
// Shared by all three input paths: dc2_poll_host_pad, PSPadBackend::readState,
// and g449PollConfiguredPad. Provides: inner/outer deadzone with smooth rescaling
// (no discontinuity), configurable power-curve response, circular magnitude
// clamping, and per-stick tuning. Rollback: DC2_STICK_LINEAR=1.

static float stickEnvFloat(const char *name, float def)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return def;
    char *end = nullptr;
    float f = std::strtof(v, &end);
    return (end != v) ? f : def;
}

struct StickParams
{
    float innerDead;    // inner deadzone radius (0..1)
    float outerDead;    // outer deadzone radius (innerDead..1)
    float leftCurve;    // response curve exponent for left stick
    float rightCurve;   // response curve exponent for right stick
    float trigThresh;   // analog trigger press threshold
    bool  linear;       // rollback: true = old linear behaviour
};

static const StickParams &getStickParams()
{
    static const StickParams p = []() {
        StickParams s;
        const char *linV = std::getenv("DC2_STICK_LINEAR");
        s.linear = linV && *linV && std::strcmp(linV, "0") != 0;

        s.innerDead = stickEnvFloat("DC2_STICK_DEADZONE", 0.15f);
        s.outerDead = stickEnvFloat("DC2_STICK_OUTER", 0.95f);
        float baseCurve = stickEnvFloat("DC2_STICK_CURVE", 2.2f);
        s.leftCurve  = stickEnvFloat("DC2_LSTICK_CURVE", baseCurve);
        s.rightCurve = stickEnvFloat("DC2_RSTICK_CURVE", baseCurve);
        s.trigThresh = stickEnvFloat("DC2_TRIGGER_THRESH", 0.0f);

        // Clamp to sane ranges.
        if (s.innerDead < 0.f) s.innerDead = 0.f;
        if (s.innerDead > 0.9f) s.innerDead = 0.9f;
        if (s.outerDead <= s.innerDead) s.outerDead = s.innerDead + 0.05f;
        if (s.outerDead > 1.0f) s.outerDead = 1.0f;
        if (s.leftCurve < 0.1f) s.leftCurve = 0.1f;
        if (s.rightCurve < 0.1f) s.rightCurve = 0.1f;

        if (!s.linear)
            std::fprintf(stderr,
                "[G7:stick] deadzone=%.2f outer=%.2f lCurve=%.1f rCurve=%.1f trigThresh=%.2f\n",
                s.innerDead, s.outerDead, s.leftCurve, s.rightCurve, s.trigThresh);
        return s;
    }();
    return p;
}

// Full stick processing: deadzone → rescale → power curve → byte.
// The scale factor is applied to the ORIGINAL per-axis values (not a normalised
// direction), so the controller's native shape is preserved — a square-range pad
// reaches 0xFF on each axis even on diagonals, matching DualShock 2 behaviour.
// curve > 1 = less sensitive near centre (2.2 typical); curve = 1 = linear.
static std::pair<uint8_t, uint8_t> processStick(float rawX, float rawY,
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
        const auto &sp = getStickParams();
        if (sp.linear) {
            // Rollback: original raw linear conversion (no deadzone).
            data[6] = static_cast<uint8_t>(128 + lx * 127);
            data[7] = static_cast<uint8_t>(128 + ly * 127);
            data[4] = static_cast<uint8_t>(128 + rx * 127);
            data[5] = static_cast<uint8_t>(128 + ry * 127);
        } else {
            auto l = processStick(lx, ly, sp.innerDead, sp.outerDead, sp.leftCurve);
            auto r = processStick(rx, ry, sp.innerDead, sp.outerDead, sp.rightCurve);
            data[6] = l.first;  data[7] = l.second;
            data[4] = r.first;  data[5] = r.second;
        }
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

    // Stick processing uses the shared pipeline (processStick / processStickLinear).
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

        // Sticks: full pipeline or legacy linear depending on rollback flag.
        const float rawLX = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        const float rawLY = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        const float rawRX = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        const float rawRY = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        if (sp.linear) {
            auto l = processStickLinear(rawLX, rawLY, 0.20f);
            auto r = processStickLinear(rawRX, rawRY, 0.20f);
            lx = l.first; ly = l.second; rx = r.first; ry = r.second;
        } else {
            auto l = processStick(rawLX, rawLY, sp.innerDead, sp.outerDead, sp.leftCurve);
            auto r = processStick(rawRX, rawRY, sp.innerDead, sp.outerDead, sp.rightCurve);
            lx = l.first; ly = l.second; rx = r.first; ry = r.second;
        }
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
        // Keyboard is binary (no curve); full-scale deflection when held.
        float kx = (IsKeyDown(KEY_D) ? 1.f : 0.f) - (IsKeyDown(KEY_A) ? 1.f : 0.f);
        float ky = (IsKeyDown(KEY_S) ? 1.f : 0.f) - (IsKeyDown(KEY_W) ? 1.f : 0.f);
        auto toByte = [](float v) -> uint8_t {
            int b = 128 + static_cast<int>(v * 127.0f);
            if (b < 0) b = 0; else if (b > 255) b = 255;
            return static_cast<uint8_t>(b);
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
