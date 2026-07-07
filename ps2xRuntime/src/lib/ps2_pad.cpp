#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <iostream>
#include <cmath>
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
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
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
    const int gamepad = firstAvailableGamepad();
    const bool padConnected = (gamepad != kNoGamepad);
    if (!padConnected && !allowKeyboard)
        return false;

    uint16_t mask = 0u; // active-high
    auto press = [&mask](uint16_t bit) { mask |= bit; };

    // Axis -> 0x80-centred byte with a radial deadzone (~0.20). raylib axes are
    // -1..+1; LEFT_Y/RIGHT_Y are +1 when the stick is pushed DOWN. The free-roam
    // movement code (F66) expects Up -> low byte, Down -> high byte, which matches
    // (axis -1 -> ~0x01, axis +1 -> ~0xFF).
    auto deflect = [](float x, float y) -> std::pair<uint8_t, uint8_t> {
        float mag = std::sqrt(x * x + y * y);
        constexpr float kDead = 0.20f;
        if (mag < kDead) { x = 0.f; y = 0.f; }
        auto toByte = [](float v) -> uint8_t {
            int b = 128 + static_cast<int>(v * 127.0f);
            if (b < 0) b = 0; else if (b > 255) b = 255;
            return static_cast<uint8_t>(b);
        };
        return { toByte(x), toByte(y) };
    };

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
        if (GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_TRIGGER)  > -0.5f) press(PAD_L2);
        if (GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > -0.5f) press(PAD_R2);

        auto l = deflect(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X),
                         GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y));
        auto r = deflect(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X),
                         GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y));
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
        float kx = (IsKeyDown(KEY_D) ? 1.f : 0.f) - (IsKeyDown(KEY_A) ? 1.f : 0.f);
        float ky = (IsKeyDown(KEY_S) ? 1.f : 0.f) - (IsKeyDown(KEY_W) ? 1.f : 0.f);
        auto l = deflect(kx, ky);
        lx = l.first; ly = l.second;
    }

    if (outMask) *outMask = mask;
    if (outLX) *outLX = lx;
    if (outLY) *outLY = ly;
    if (outRX) *outRX = rx;
    if (outRY) *outRY = ry;
    return true;
}
