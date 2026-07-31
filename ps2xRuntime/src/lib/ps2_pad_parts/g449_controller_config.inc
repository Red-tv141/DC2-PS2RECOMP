// ============================================================================
// G449 — LAUNCHER CONTROLLER CONFIG (DC2_CONTROLLER_CONFIG=<path to controller.json>)
//
// The launcher owns the remapping UI and writes a controller.json describing all 24 PS2
// controls plus both stick dead zones. This applies it inside the runner, replacing the
// hardcoded raylib mapping in dc2_poll_host_pad for as long as a config is present.
//
// Two binding vocabularies come out of the launcher and both must be honoured:
//   * "Keyboard" bindings use WPF System.Windows.Input.Key NAMES ("Up", "W", "Space",
//     "Return", "Back", "D1", "NumPad8", "LeftShift", ...)
//   * "XInput" bindings use the launcher's own tokens ("XInput_A", "XInput_LB",
//     "XInput_DpadUp", "XInput_LT", "XInput_LeftStick_Up", ...)
// Neither is a raylib enum, so each is translated here once, at load, into a resolved
// binding the per-frame poll can evaluate without any string work.
//
// STICK CONTROLS ARE NOT BUTTONS. LeftStickUp/Down/Left/Right (and the Right equivalents)
// produce ANALOG deflection. When such a control is bound to a physical stick direction
// the real axis value is used, so analog walking is preserved; when it is bound to a key
// or a digital button the deflection is full-scale, which is what a keyboard can express.
// This is why the poll cannot simply OR together a set of button bits.
//
// No config file => this whole unit reports "inactive" and the original mapping runs
// untouched. Parsing is done once (the file is small and read at startup only).
// ============================================================================

namespace
{
    // The 24 controls, in the launcher's Ps2Button enum order. Index is the wire format
    // between the two halves of this file; the names are the JSON keys.
    enum G449Control
    {
        kG449DpadUp = 0, kG449DpadDown, kG449DpadLeft, kG449DpadRight,
        kG449LeftStickUp, kG449LeftStickDown, kG449LeftStickLeft, kG449LeftStickRight, kG449L3,
        kG449RightStickUp, kG449RightStickDown, kG449RightStickLeft, kG449RightStickRight, kG449R3,
        kG449Cross, kG449Circle, kG449Square, kG449Triangle,
        kG449L1, kG449L2, kG449R1, kG449R2,
        kG449Start, kG449Select,
        kG449ControlCount
    };

    const char *const kG449ControlNames[kG449ControlCount] = {
        "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
        "LeftStickUp", "LeftStickDown", "LeftStickLeft", "LeftStickRight", "L3",
        "RightStickUp", "RightStickDown", "RightStickLeft", "RightStickRight", "R3",
        "Cross", "Circle", "Square", "Triangle",
        "L1", "L2", "R1", "R2",
        "Start", "Select"
    };

    // scePad active-high bit for each control. The eight stick-direction controls have no
    // button bit — they feed the analog bytes instead.
    const uint16_t kG449ControlBit[kG449ControlCount] = {
        PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
        0, 0, 0, 0, PAD_L3,
        0, 0, 0, 0, PAD_R3,
        PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
        PAD_L1, PAD_L2, PAD_R1, PAD_R2,
        PAD_START, PAD_SELECT
    };

    enum G449Source
    {
        kG449SrcNone = 0,
        kG449SrcKey,        // raylib KEY_* code
        kG449SrcPadButton,  // raylib GAMEPAD_BUTTON_*
        kG449SrcPadAxis     // raylib GAMEPAD_AXIS_* with a required sign
    };

    struct G449Binding
    {
        int source = kG449SrcNone;
        int code = 0;      // key code / button id / axis id
        float sign = 1.0f; // axis only: +1 wants a positive deflection, -1 negative
    };

    // ---- launcher token -> raylib -------------------------------------------

    // WPF Key names the launcher can emit. Only the ones raylib can observe are listed;
    // anything unknown resolves to "unbound" and is reported once at load.
    int g449KeyFromWpfName(const std::string &n)
    {
        struct Row { const char *name; int key; };
        static const Row rows[] = {
            {"Up", KEY_UP}, {"Down", KEY_DOWN}, {"Left", KEY_LEFT}, {"Right", KEY_RIGHT},
            {"Space", KEY_SPACE}, {"Return", KEY_ENTER}, {"Enter", KEY_ENTER},
            {"Escape", KEY_ESCAPE}, {"Back", KEY_BACKSPACE}, {"Tab", KEY_TAB},
            {"LeftShift", KEY_LEFT_SHIFT}, {"RightShift", KEY_RIGHT_SHIFT},
            {"LeftCtrl", KEY_LEFT_CONTROL}, {"RightCtrl", KEY_RIGHT_CONTROL},
            {"LeftAlt", KEY_LEFT_ALT}, {"RightAlt", KEY_RIGHT_ALT},
            {"OemComma", KEY_COMMA}, {"OemPeriod", KEY_PERIOD},
            {"OemMinus", KEY_MINUS}, {"OemPlus", KEY_EQUAL},
            {"OemQuestion", KEY_SLASH}, {"OemSemicolon", KEY_SEMICOLON},
            {"OemOpenBrackets", KEY_LEFT_BRACKET}, {"OemCloseBrackets", KEY_RIGHT_BRACKET},
            {"OemBackslash", KEY_BACKSLASH}, {"OemPipe", KEY_BACKSLASH},
            {"OemQuotes", KEY_APOSTROPHE}, {"OemTilde", KEY_GRAVE},
            {"NumPad0", KEY_KP_0}, {"NumPad1", KEY_KP_1}, {"NumPad2", KEY_KP_2},
            {"NumPad3", KEY_KP_3}, {"NumPad4", KEY_KP_4}, {"NumPad5", KEY_KP_5},
            {"NumPad6", KEY_KP_6}, {"NumPad7", KEY_KP_7}, {"NumPad8", KEY_KP_8},
            {"NumPad9", KEY_KP_9},
            {"Insert", KEY_INSERT}, {"Delete", KEY_DELETE}, {"Home", KEY_HOME},
            {"End", KEY_END}, {"PageUp", KEY_PAGE_UP}, {"PageDown", KEY_PAGE_DOWN},
        };
        for (const Row &r : rows)
            if (n == r.name) return r.key;

        // "A".."Z" and the digit form "D0".."D9", plus "F1".."F12".
        if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z')
            return KEY_A + (n[0] - 'A');
        if (n.size() == 2 && n[0] == 'D' && n[1] >= '0' && n[1] <= '9')
            return (n[1] == '0') ? KEY_ZERO : (KEY_ONE + (n[1] - '1'));
        if (n.size() >= 2 && n[0] == 'F')
        {
            const int idx = std::atoi(n.c_str() + 1);
            if (idx >= 1 && idx <= 12) return KEY_F1 + (idx - 1);
        }
        return 0;
    }

    bool g449BindingFromToken(const std::string &device, const std::string &token,
                              G449Binding &out)
    {
        if (token.empty())
            return false;

        const bool isXInput = (token.rfind("XInput_", 0) == 0) ||
                              (device == "XInput") || (device == "Gamepad");
        if (!isXInput)
        {
            const int key = g449KeyFromWpfName(token);
            if (key == 0) return false;
            out.source = kG449SrcKey;
            out.code = key;
            return true;
        }

        struct BtnRow { const char *tok; int btn; };
        static const BtnRow btns[] = {
            {"XInput_A", GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
            {"XInput_B", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
            {"XInput_X", GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
            {"XInput_Y", GAMEPAD_BUTTON_RIGHT_FACE_UP},
            {"XInput_LB", GAMEPAD_BUTTON_LEFT_TRIGGER_1},
            {"XInput_RB", GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
            {"XInput_LT", GAMEPAD_BUTTON_LEFT_TRIGGER_2},
            {"XInput_RT", GAMEPAD_BUTTON_RIGHT_TRIGGER_2},
            {"XInput_Start", GAMEPAD_BUTTON_MIDDLE_RIGHT},
            {"XInput_Back", GAMEPAD_BUTTON_MIDDLE_LEFT},
            {"XInput_LS", GAMEPAD_BUTTON_LEFT_THUMB},
            {"XInput_RS", GAMEPAD_BUTTON_RIGHT_THUMB},
            {"XInput_DpadUp", GAMEPAD_BUTTON_LEFT_FACE_UP},
            {"XInput_DpadDown", GAMEPAD_BUTTON_LEFT_FACE_DOWN},
            {"XInput_DpadLeft", GAMEPAD_BUTTON_LEFT_FACE_LEFT},
            {"XInput_DpadRight", GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
        };
        for (const BtnRow &r : btns)
            if (token == r.tok)
            {
                out.source = kG449SrcPadButton;
                out.code = r.btn;
                return true;
            }

        // Stick directions. raylib's Y axes are +1 when the stick is pushed DOWN, which is
        // why "Up" wants sign -1 -- the same convention the original deflect() relies on.
        struct AxisRow { const char *tok; int axis; float sign; };
        static const AxisRow axes[] = {
            {"XInput_LeftStick_Left",   GAMEPAD_AXIS_LEFT_X,  -1.0f},
            {"XInput_LeftStick_Right",  GAMEPAD_AXIS_LEFT_X,  +1.0f},
            {"XInput_LeftStick_Up",     GAMEPAD_AXIS_LEFT_Y,  -1.0f},
            {"XInput_LeftStick_Down",   GAMEPAD_AXIS_LEFT_Y,  +1.0f},
            {"XInput_RightStick_Left",  GAMEPAD_AXIS_RIGHT_X, -1.0f},
            {"XInput_RightStick_Right", GAMEPAD_AXIS_RIGHT_X, +1.0f},
            {"XInput_RightStick_Up",    GAMEPAD_AXIS_RIGHT_Y, -1.0f},
            {"XInput_RightStick_Down",  GAMEPAD_AXIS_RIGHT_Y, +1.0f},
        };
        for (const AxisRow &r : axes)
            if (token == r.tok)
            {
                out.source = kG449SrcPadAxis;
                out.code = r.axis;
                out.sign = r.sign;
                return true;
            }
        return false;
    }

    // ---- the config ---------------------------------------------------------

    struct G449ControllerConfig
    {
        bool active = false;
        G449Binding bind[kG449ControlCount];
        float leftDead = 0.10f;
        float rightDead = 0.10f;
    };

    // Minimal targeted JSON reader. The file is written by exactly one producer
    // (ControllerConfigService, System.Text.Json, camelCase) and is tiny, so scanning for
    // the fields we need is both sufficient and dependency-free -- the runtime has no JSON
    // library and pulling one in for 26 values would be the wrong trade.
    bool g449ExtractString(const std::string &src, size_t from, size_t to,
                           const char *field, std::string &out)
    {
        const std::string needle = std::string("\"") + field + "\"";
        const size_t k = src.find(needle, from);
        if (k == std::string::npos || k >= to) return false;
        const size_t colon = src.find(':', k + needle.size());
        if (colon == std::string::npos || colon >= to) return false;
        const size_t q1 = src.find('"', colon + 1);
        if (q1 == std::string::npos || q1 >= to) return false;
        const size_t q2 = src.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > to) return false;
        out = src.substr(q1 + 1, q2 - q1 - 1);
        return true;
    }

    bool g449ExtractNumber(const std::string &src, const char *field, double &out)
    {
        const std::string needle = std::string("\"") + field + "\"";
        const size_t k = src.find(needle);
        if (k == std::string::npos) return false;
        const size_t colon = src.find(':', k + needle.size());
        if (colon == std::string::npos) return false;
        out = std::strtod(src.c_str() + colon + 1, nullptr);
        return true;
    }

    const G449ControllerConfig &g449Config()
    {
        static const G449ControllerConfig cfg = []() {
            G449ControllerConfig c;
            const char *path = std::getenv("DC2_CONTROLLER_CONFIG");
            if (path == nullptr || *path == '\0')
                return c;

            std::ifstream f(path, std::ios::binary);
            if (!f)
            {
                std::fprintf(stderr, "[G449:pad] controller config not readable: %s\n", path);
                return c;
            }
            const std::string src((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());

            double dz = 0.0;
            if (g449ExtractNumber(src, "leftStickDeadZonePercent", dz))
                c.leftDead = static_cast<float>(dz / 100.0);
            if (g449ExtractNumber(src, "rightStickDeadZonePercent", dz))
                c.rightDead = static_cast<float>(dz / 100.0);
            if (c.leftDead < 0.f || c.leftDead > 0.9f) c.leftDead = 0.10f;
            if (c.rightDead < 0.f || c.rightDead > 0.9f) c.rightDead = 0.10f;

            int bound = 0, unresolved = 0;
            for (int i = 0; i < kG449ControlCount; ++i)
            {
                // Each control is an object keyed by its name; bound the search to that
                // object so a later control's fields cannot be read for an earlier one.
                const std::string key = std::string("\"") + kG449ControlNames[i] + "\"";
                const size_t k = src.find(key);
                if (k == std::string::npos) continue;
                const size_t objOpen = src.find('{', k);
                if (objOpen == std::string::npos) continue;
                const size_t objClose = src.find('}', objOpen);
                if (objClose == std::string::npos) continue;

                std::string device, token;
                g449ExtractString(src, objOpen, objClose, "deviceType", device);
                if (!g449ExtractString(src, objOpen, objClose, "keyOrButton", token))
                    continue;

                G449Binding b;
                if (g449BindingFromToken(device, token, b))
                {
                    c.bind[i] = b;
                    ++bound;
                }
                else
                {
                    ++unresolved;
                    std::fprintf(stderr, "[G449:pad] unmapped binding %s = '%s' (%s)\n",
                                 kG449ControlNames[i], token.c_str(), device.c_str());
                }
            }

            if (bound == 0)
            {
                std::fprintf(stderr, "[G449:pad] config had no usable bindings: %s\n", path);
                return c;
            }
            c.active = true;
            std::fprintf(stderr,
                         "[G449:pad] controller config active: %d bound, %d unmapped, "
                         "deadzone L=%.0f%% R=%.0f%% (%s)\n",
                         bound, unresolved, c.leftDead * 100.f, c.rightDead * 100.f, path);
            return c;
        }();
        return cfg;
    }

    // ---- per-frame evaluation ------------------------------------------------

    // Digital state of a binding. `gamepad` is kNoGamepad when no pad is connected, in
    // which case only keyboard bindings can fire.
    bool g449BindingDown(const G449Binding &b, int gamepad)
    {
        switch (b.source)
        {
        case kG449SrcKey:
            return IsKeyDown(b.code);
        case kG449SrcPadButton:
            return gamepad != kNoGamepad && IsGamepadButtonDown(gamepad, b.code);
        case kG449SrcPadAxis:
        {
            if (gamepad == kNoGamepad) return false;
            const float v = GetGamepadAxisMovement(gamepad, b.code) * b.sign;
            return v > 0.5f;
        }
        default:
            return false;
        }
    }

    // Analog magnitude of a binding in 0..1. A stick binding gives the real deflection; a
    // key or button gives full scale. This is what preserves analog walking while still
    // letting the stick be remapped onto keys.
    float g449BindingAxis(const G449Binding &b, int gamepad)
    {
        if (b.source == kG449SrcPadAxis)
        {
            if (gamepad == kNoGamepad) return 0.f;
            const float v = GetGamepadAxisMovement(gamepad, b.code) * b.sign;
            return (v > 0.f) ? ((v > 1.f) ? 1.f : v) : 0.f;
        }
        return g449BindingDown(b, gamepad) ? 1.0f : 0.f;
    }

    // Resolve one stick from its four direction bindings into 0x80-centred bytes.
    void g449Stick(const G449ControllerConfig &c, int gamepad, int up, int down,
                   int left, int right, float dead, uint8_t *outX, uint8_t *outY)
    {
        float x = g449BindingAxis(c.bind[right], gamepad) -
                  g449BindingAxis(c.bind[left], gamepad);
        float y = g449BindingAxis(c.bind[down], gamepad) -
                  g449BindingAxis(c.bind[up], gamepad);

        const float mag = std::sqrt(x * x + y * y);
        if (mag < dead) { x = 0.f; y = 0.f; }

        auto toByte = [](float v) -> uint8_t {
            int b = 128 + static_cast<int>(v * 127.0f);
            if (b < 0) b = 0; else if (b > 255) b = 255;
            return static_cast<uint8_t>(b);
        };
        *outX = toByte(x);
        *outY = toByte(y);
    }

    // Full poll under a launcher config. Returns false when no config is active, so the
    // caller falls through to the original built-in mapping unchanged.
    bool g449PollConfiguredPad(uint16_t *outMask, uint8_t *outLX, uint8_t *outLY,
                               uint8_t *outRX, uint8_t *outRY)
    {
        const G449ControllerConfig &c = g449Config();
        if (!c.active)
            return false;

        const int gamepad = firstAvailableGamepad();

        uint16_t mask = 0u;
        for (int i = 0; i < kG449ControlCount; ++i)
        {
            const uint16_t bit = kG449ControlBit[i];
            if (bit != 0u && g449BindingDown(c.bind[i], gamepad))
                mask |= bit;
        }

        uint8_t lx = kPadStickCenter, ly = kPadStickCenter;
        uint8_t rx = kPadStickCenter, ry = kPadStickCenter;
        g449Stick(c, gamepad, kG449LeftStickUp, kG449LeftStickDown,
                  kG449LeftStickLeft, kG449LeftStickRight, c.leftDead, &lx, &ly);
        g449Stick(c, gamepad, kG449RightStickUp, kG449RightStickDown,
                  kG449RightStickLeft, kG449RightStickRight, c.rightDead, &rx, &ry);

        if (outMask) *outMask = mask;
        if (outLX) *outLX = lx;
        if (outLY) *outLY = ly;
        if (outRX) *outRX = rx;
        if (outRY) *outRY = ry;
        return true;
    }
} // namespace
