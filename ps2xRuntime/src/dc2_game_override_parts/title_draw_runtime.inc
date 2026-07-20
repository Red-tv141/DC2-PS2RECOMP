// G93: forward decl (defined after g67_title_scope). Makes the title-rock renderinfo
// naturally correct for the NATIVE transform path: focal 480 + follow-camera eye, BEFORE
// TitleDraw's mgSetRenderInfo / TitleMapDraw's GetCameraMatrix run this frame.
static void g93_title_renderinfo_fix(uint8_t *rdram, R5900Context *ctx);
static bool g67_title_scope(uint8_t *rdram, R5900Context *ctx);

static void f29_title_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    // G93: correct the title-bg projection focal + follow-camera eye BEFORE the real
    // TitleDraw builds renderinfo (mgSetRenderInfo) and TitleMapDraw reads the camera
    // (GetCameraMatrix). This is the upstream Option-3 fix that lets the transform path
    // land on-screen (replacing the DC2_G92_FORCEMTX constant-injection lever).
    g93_title_renderinfo_fix(rdram, ctx);
    f29_log_title_draw_state("TitleDraw.enter", n, rdram, ctx, runtime);
    TitleDraw__Fv_0x2a0ab0(rdram, ctx, runtime);
    f29_log_title_draw_state("TitleDraw.exit", n, rdram, ctx, runtime);
}

static void f29_title_mode_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f29_log_title_draw_state("TitleModeDraw.enter", n, rdram, ctx, runtime);
    // G59: the direct TitleModeDraw probe calls recompiled draw bodies directly.
    // Suppress back-edge yields for this title-frame draw so nested resume PCs do
    // not leak out through the probe path and unwind main after one frame.
    static const bool g57SuppressDisabled = dc2_env_flag_enabled("DC2_G57_NO_SUPPRESS");
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    TitleModeDraw__Fv_0x2a1b60(rdram, ctx, runtime);
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    f29_log_title_draw_state("TitleModeDraw.exit", n, rdram, ctx, runtime);
}

static void f29_primquad_drawprim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t rect = getRegU32(ctx, 5);
    const uint32_t x = dc2_read_u32(rdram, rect + 0u);
    const uint32_t y = dc2_read_u32(rdram, rect + 4u);
    const uint32_t w = dc2_read_u32(rdram, rect + 8u);
    const uint32_t h = dc2_read_u32(rdram, rect + 12u);

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:PrimQuadPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x rect=0x%x xywh=%d,%d,%d,%d f12=%g f13=%g dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, rect,
                     static_cast<int32_t>(x), static_cast<int32_t>(y),
                     static_cast<int32_t>(w), static_cast<int32_t>(h),
                     static_cast<double>(ctx->f[12]), static_cast<double>(ctx->f[13]),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    PrimQuad__FP11mgCDrawPrimff9mgRect_i__0x21fe60(rdram, ctx, runtime);
    f29_log_counters("PrimQuadPrim.exit", n, runtime, ctx);
}

static void f29_primquad_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t tex = getRegU32(ctx, 4);
    const uint32_t rect = getRegU32(ctx, 5);
    const uint32_t alpha = getRegU32(ctx, 6);
    const uint32_t red = getRegU32(ctx, 7);
    const uint32_t green = getRegU32(ctx, 8);
    const uint32_t blue = getRegU32(ctx, 9);
    const uint32_t x = dc2_read_u32(rdram, rect + 0u);
    const uint32_t y = dc2_read_u32(rdram, rect + 4u);
    const uint32_t w = dc2_read_u32(rdram, rect + 8u);
    const uint32_t h = dc2_read_u32(rdram, rect + 12u);

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:PrimQuadTex.enter] n=%u pc=0x%x ra=0x%x tex=0x%x rect=0x%x xywh=%d,%d,%d,%d rgba=%u,%u,%u,%u f12=%g f13=%g dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), tex, rect,
                     static_cast<int32_t>(x), static_cast<int32_t>(y),
                     static_cast<int32_t>(w), static_cast<int32_t>(h),
                     red, green, blue, alpha,
                     static_cast<double>(ctx->f[12]), static_cast<double>(ctx->f[13]),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    PrimQuad__FP10mgCTextureff9mgRect_i_iiii_0x21ff30(rdram, ctx, runtime);
    f29_log_counters("PrimQuadTex.exit", n, runtime, ctx);
}

static void f33_color_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f33_trace_drawprim_enabled() && (n <= 128u || (n % 1024u) == 0u))
    {
        const uint32_t prim = getRegU32(ctx, 4);
        const uint32_t out = dc2_read_u32(rdram, prim + 0xDCu);
        const uint32_t qBits = dc2_read_u32(rdram, prim + 0xF8u);
        std::fprintf(stderr,
                     "[F33:Color.call] n=%u pc=0x%x ra=0x%x prim=0x%x out=0x%x rgba=%u,%u,%u,%u q=0x%x\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, out,
                     getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8), qBits);
    }

    Color__11mgCDrawPrimFiiii_0x134c80(rdram, ctx, runtime);
}

static void f33_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f33_trace_drawprim_enabled() && (n <= 128u || (n % 1024u) == 0u))
    {
        const uint32_t prim = getRegU32(ctx, 4);
        const uint32_t tex = getRegU32(ctx, 5);
        const uint32_t out = dc2_read_u32(rdram, prim + 0xDCu);
        const uint64_t tex0 = tex ? dc2_read_u64(rdram, tex + 0x38u) : 0u;
        const uint64_t tex1 = tex ? dc2_read_u64(rdram, tex + 0x40u) : 0u;
        std::fprintf(stderr,
                     "[F33:Texture.call] n=%u pc=0x%x ra=0x%x prim=0x%x tex=0x%x out=0x%x tex0=0x%016llx tex1=0x%016llx dims=%u,%u,%u,%u\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, tex, out,
                     static_cast<unsigned long long>(tex0),
                     static_cast<unsigned long long>(tex1),
                     static_cast<uint32_t>(dc2_read_u32(rdram, tex + 0u) & 0xFFFFu),
                     static_cast<uint32_t>((dc2_read_u32(rdram, tex + 0u) >> 16u) & 0xFFFFu),
                     static_cast<uint32_t>(dc2_read_u32(rdram, tex + 4u) & 0xFFFFu),
                     static_cast<uint32_t>((dc2_read_u32(rdram, tex + 4u) >> 16u) & 0xFFFFu));
    }

    Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(rdram, ctx, runtime);
}

static void f29_begin_prim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:BeginPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x mode=%u dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    Begin__11mgCDrawPrimFi_0x1344a0(rdram, ctx, runtime);
}

static void f29_end_prim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:EndPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 4),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    End__11mgCDrawPrimFv_0x134690(rdram, ctx, runtime);
    f29_log_counters("EndPrim.exit", n, runtime, ctx);
}

// PHASE F40: scriptable headless pad-input injector.
static uint16_t f40_btn_bit(const char *name, size_t len)
{
    struct Entry { const char *name; uint16_t bit; };
    static const Entry kMap[] = {
        {"Select",   0x0001u}, {"L3",     0x0002u}, {"R3",     0x0004u},
        {"Start",    0x0008u}, {"Up",     0x0010u}, {"Right",  0x0020u},
        {"Down",     0x0040u}, {"Left",   0x0080u}, {"L2",     0x0100u},
        {"R2",       0x0200u}, {"L1",     0x0400u}, {"R1",     0x0800u},
        {"Triangle", 0x1000u}, {"Circle", 0x2000u},
        // DC2 menu checks the R1 bit (0x0800) for confirm via MenuCheckPushButton.
        // Keep scripted Cross compatible with canonical prompts by asserting both.
        {"Cross",    0x4800u},
        {"Square",   0x8000u},
        // Global debug MenuLoop uses raw CGamePad masks directly:
        // cursor down/up = 0x4000/0x1000, value +/- = 0x2000/0x8000,
        // confirm = 0x20. These aliases avoid overloading physical button names.
        {"DebugDown",     0x4000u}, {"DebugUp",       0x1000u},
        {"DebugInc",      0x2000u}, {"DebugDec",      0x8000u},
        {"DebugConfirm",  0x0020u}, {"DebugCancel",   0x0400u},
        {"DebugPlus10",   0x0008u}, {"DebugMinus10",  0x0004u},
        {"DebugPlus100",  0x0002u}, {"DebugMinus100", 0x0001u},
    };
    for (const auto &e : kMap)
        if (std::strlen(e.name) == len && std::strncmp(name, e.name, len) == 0)
            return e.bit;
    // Raw hex mask: "0x1234" ג€” lets replay_from_rec.py emit exact recorded bits
    // without aliasing through named buttons (e.g. Cross=0x4800 vs raw 0x4000).
    if (len > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return static_cast<uint16_t>(std::strtoul(name, nullptr, 16));
    return 0u;
}

static const std::vector<std::pair<uint32_t, uint16_t>> &f40_get_events()
{
    static std::vector<std::pair<uint32_t, uint16_t>> events = []() {
        std::vector<std::pair<uint32_t, uint16_t>> ev;
        const char *spec = std::getenv("DC2_PAD_INPUT");
        if (!spec || !*spec)
        {
            // Keep default smoke progression deterministic in headless runs.
            spec = "30..39:R1;120..129:Cross";
        }
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;

            uint32_t rangeStart = firstFrame;
            uint32_t rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart)
                {
                    const uint32_t tmp = rangeStart;
                    rangeStart = rangeEnd;
                    rangeEnd = tmp;
                }
                p = end + 1;
            }
            else if (*end == ':')
            {
                p = end + 1;
            }
            else
            {
                break;
            }

            uint16_t pressed = 0u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                pressed |= f40_btn_bit(p, static_cast<size_t>(sep - p));
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            if (pressed != 0u)
            {
                for (uint32_t frame = rangeStart; frame <= rangeEnd; ++frame)
                {
                    bool merged = false;
                    for (auto &e : ev)
                        if (e.first == frame) { e.second |= pressed; merged = true; break; }
                    if (!merged) ev.emplace_back(frame, pressed);

                    if (frame == 0xFFFFFFFFu)
                        break;
                }
            }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

// G49: parse DC2_RSTICK into per-frame right-stick (rx,ry) byte pairs. Dir tokens deflect
// the right stick: RRight rx=0xFF, RLeft rx=0x00, RUp ry=0x00, RDown ry=0xFF (0x80 centre).
struct F40RStick { uint32_t frame; uint8_t rx; uint8_t ry; };
static const std::vector<F40RStick> &f40_get_rstick_events()
{
    static std::vector<F40RStick> events = []() {
        std::vector<F40RStick> ev;
        const char *spec = std::getenv("DC2_RSTICK");
        if (!spec || !*spec) return ev;
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;
            uint32_t rangeStart = firstFrame, rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart) std::swap(rangeStart, rangeEnd);
                p = end + 1;
            }
            else if (*end == ':') { p = end + 1; }
            else break;
            uint8_t rx = 0x80u, ry = 0x80u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                const size_t len = static_cast<size_t>(sep - p);
                if (len == 6 && std::strncmp(p, "RRight", 6) == 0) rx = 0xFFu;
                else if (len == 5 && std::strncmp(p, "RLeft", 5) == 0) rx = 0x00u;
                else if (len == 3 && std::strncmp(p, "RUp", 3) == 0)  ry = 0x00u;
                else if (len == 5 && std::strncmp(p, "RDown", 5) == 0) ry = 0xFFu;
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            for (uint32_t f = rangeStart; f <= rangeEnd; ++f)
            {
                ev.push_back({f, rx, ry});
                if (f == 0xFFFFFFFFu) break;
            }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

static uint16_t f40_scripted_press_for_frame_range(uint32_t firstFrame, uint32_t lastFrame)
{
    if (firstFrame > lastFrame)
    {
        const uint32_t tmp = firstFrame;
        firstFrame = lastFrame;
        lastFrame = tmp;
    }

    uint16_t pressed = 0u;
    for (const auto &e : f40_get_events())
        if (e.first >= firstFrame && e.first <= lastFrame) pressed |= e.second;
    return pressed;
}

static bool f40_script_input_requested()
{
    return !f40_get_events().empty();
}

// G7: true only when DC2_PAD_INPUT was set EXPLICITLY (not the headless default
// smoke script). An explicit script suppresses the live controller so deterministic
// automation stays reproducible even with a pad plugged in.
static bool f40_explicit_script()
{
    static const bool e = []() {
        const char *s = std::getenv("DC2_PAD_INPUT");
        return s != nullptr && *s != '\0';
    }();
    return e;
}

// F66: in-dungeon free-roam pad driver. The title/menu input clock (scriptFrame,
// derived from the mgEndFrame OVERRIDE call count) FREEZES once in the dungeon,
// because the dungeon loop calls mgEndFrame through a direct jal that bypasses the
// registered override hook (so f40_drive_pad stops being called and no scripted
// input reaches free-roam). This driver runs off the host PRESENT loop instead
// (which ticks every frame in all game states) with its own counter that starts at
// free-roam entry, and reads a SEPARATE env DC2_DUNGEON_PAD (same
// "start..end:Alias[+Alias];..." range syntax) so it never replays the navigation
// frames. Default empty => inert. Movement uses the LEFT ANALOG STICK like the
// title path's F66 fix (free-roam reads Analog__11CPadControl, not the D-pad).
static const std::vector<std::pair<uint32_t, uint16_t>> &f66_get_dungeon_events()
{
    static std::vector<std::pair<uint32_t, uint16_t>> events = []() {
        std::vector<std::pair<uint32_t, uint16_t>> ev;
        const char *spec = std::getenv("DC2_DUNGEON_PAD");
        if (!spec || !*spec) return ev; // inert unless explicitly requested
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;
            uint32_t rangeStart = firstFrame, rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart) { const uint32_t t = rangeStart; rangeStart = rangeEnd; rangeEnd = t; }
                p = end + 1;
            }
            else if (*end == ':') { p = end + 1; }
            else break;
            uint16_t pressed = 0u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                pressed |= f40_btn_bit(p, static_cast<size_t>(sep - p));
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            if (pressed != 0u)
                for (uint32_t frame = rangeStart; frame <= rangeEnd; ++frame)
                {
                    bool merged = false;
                    for (auto &e : ev) if (e.first == frame) { e.second |= pressed; merged = true; break; }
                    if (!merged) ev.emplace_back(frame, pressed);
                    if (frame == 0xFFFFFFFFu) break;
                }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

// Definition of the present-loop entry point is at GLOBAL scope below the anonymous
// namespace (it must have external linkage so ps2_runtime.cpp can call it).

static void f40_log_inject(uint32_t frame, uint16_t pressed, const char *source)
{
    static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
    if (!trace || pressed == 0u)
        return;

    std::fprintf(stderr, "[F40:inject] frame=%u pressed=0x%04x activeLow=0x%04x source=%s\n",
                 frame, static_cast<unsigned>(pressed),
                 static_cast<unsigned>(~pressed & 0xFFFFu),
                 source ? source : "unknown");
    std::fflush(stderr);
}

static void f40_drive_pad(uint32_t frame, uint32_t loopNo)
{
    static bool havePrevFrame = false;
    static uint32_t prevFrame = 0u;
    static uint16_t deferredPress = 0u;

    // F64: correlate the input scriptFrame with the top-level LoopNo (program
    // state: title/menu/costume/cutscene) so route-B pad timing can be tuned to
    // the actual screen transitions. Logs on every LoopNo change + periodically.
    // Gated DC2_TRACE_F64.
    {
        static const bool s_f64 = dc2_env_flag_enabled("DC2_TRACE_F64");
        if (s_f64)
        {
            static uint32_t s_prevLoop = 0xFFFFFFFFu;
            static uint32_t s_lastLog = 0u;
            if (loopNo != s_prevLoop || frame - s_lastLog >= 15u)
            {
                std::fprintf(stderr, "[F64:loop] frame=%u LoopNo=%u%s\n",
                             frame, loopNo, (loopNo != s_prevLoop) ? "  <== CHANGE" : "");
                s_prevLoop = loopNo;
                s_lastLog = frame;
            }
        }
    }

    if (!f40_script_input_requested()) return;

    uint32_t rangeStart = frame;
    if (havePrevFrame)
    {
        if (frame >= prevFrame)
            rangeStart = prevFrame + 1u;
        else
            rangeStart = frame;
    }

    uint16_t effectivePressed = f40_scripted_press_for_frame_range(rangeStart, frame);

    // G49: latch the scripted right-stick deflection for this scriptFrame range (last match
    // wins). Held across frames with no button press, so the model can be turned for several
    // frames to validate the per-frame costume Z clear under rotation.
    {
        uint8_t rx = 0x80u, ry = 0x80u;
        bool found = false;
        for (const auto &e : f40_get_rstick_events())
            if (e.frame >= rangeStart && e.frame <= frame) { rx = e.rx; ry = e.ry; found = true; }
        if (found) { g_f40_rx = rx; g_f40_ry = ry; }
        else { g_f40_rx = 0x80u; g_f40_ry = 0x80u; }
    }

    // Keep Cross-window confirmations from being consumed by TitleModeKey.
    if ((effectivePressed & 0x4000u) != 0u && loopNo == 1u)
    {
        deferredPress |= effectivePressed;
        effectivePressed = 0u;
    }
    else if (deferredPress != 0u && loopNo != 1u)
    {
        effectivePressed |= deferredPress;
        deferredPress = 0u;
    }

    if (effectivePressed != 0u)
    {
        g_f40_active  = true;
        g_f40_pressed = effectivePressed;
        // F66: in-dungeon free-roam moves the player via the LEFT ANALOG STICK
        // (RunScript__12CActionChara reads Analog__11CPadControl(...,4/5)), NOT the
        // digital D-pad ג€” so a scripted "Up" with the stick centred (0x80) never
        // translated the player. Deflect the left stick to match any scripted
        // direction bit (kept ALONGSIDE the digital bit so menus that read D-pad
        // still navigate; menus ignore the analog stick). scePad bits: Up 0x10,
        // Right 0x20, Down 0x40, Left 0x80. Stick byte: centre 0x80, min 0x00,
        // max 0xFF; up = -Y = 0x00, left = -X = 0x00 (raylib axisToByte convention).
        uint8_t lx = 0x80u, ly = 0x80u;
        if ((effectivePressed & 0x10u) != 0u) ly = 0x00u; // Up
        if ((effectivePressed & 0x40u) != 0u) ly = 0xFFu; // Down
        if ((effectivePressed & 0x80u) != 0u) lx = 0x00u; // Left
        if ((effectivePressed & 0x20u) != 0u) lx = 0xFFu; // Right
        ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~effectivePressed & 0xFFFFu),
                                       lx, ly, 0x80u, 0x80u);
        f40_log_inject(frame, effectivePressed, "mgEndFrameScaled");
    }
    else
    {
        g_f40_active  = false;
        g_f40_pressed = 0u;
        ps2_stubs::clearPadOverrideState();
    }

    havePrevFrame = true;
    prevFrame = frame;
}

static bool f30_valid_frame(const GSFrameReg &frame)
{
    return frame.fbp != 0u && frame.fbw != 0u;
}

// PHASE F49: instrument WaitVSync pacing and clamp anomalous mgEndFrame wait
// counts on the known callsite (ra=0x142A08). The game's frame-rate scalar at
// gp-0x78A0 is expected to be 1 or 2 in title/menu paths.
static void f49_waitvsync_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t clamps = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t frameRate = dc2_read_u32(rdram, gp + 0xFFFF8760u); // -0x78A0
    const uint32_t startCount = getRegU32(ctx, 4);
    const uint32_t requested = getRegU32(ctx, 5);
    uint32_t effective = requested;
    bool clamped = false;

    if (ra == 0x142A08u && effective > 2u)
    {
        effective = 2u;
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(effective)));
        clamped = true;
        ++clamps;
    }

    const int32_t envMax = f49_waitvsync_env_max_count();
    if (envMax >= 0 && effective > static_cast<uint32_t>(envMax))
    {
        effective = static_cast<uint32_t>(envMax);
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(effective)));
        clamped = true;
        ++clamps;
    }

    const auto t0 = std::chrono::steady_clock::now();
    WaitVSync__Fii_0x1412a0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_waitvsync_enabled() && (clamped || n <= 16u || (n % 60u) == 0u))
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t deltaVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:wait] n=%u ra=0x%x frameRate=%u start=%u req=%u eff=%u dv=%u pre=%u post=%u dtMs=%lld clamped=%u clamps=%u\n",
                     n, ra, frameRate, startCount, requested, effective,
                     deltaVsync, preVsync, postVsync,
                     static_cast<long long>(dtMs),
                     clamped ? 1u : 0u, clamps);
    }
}

static void f49_vsync_callback_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t tickArg = getRegU32(ctx, 4);
    const uint32_t preCount = dc2_read_u32(rdram, gp + 0xFFFF8850u);   // -0x77B0
    const uint32_t preCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu);      // -0x77A4
    const uint32_t preInCb = dc2_read_u32(rdram, gp + 0xFFFF8860u);    // -0x77A0
    const uint32_t preField = dc2_read_u32(rdram, gp + 0xFFFF87B8u);   // -0x7848
    const auto t0 = std::chrono::steady_clock::now();
    VSyncCallBack__Fi_0x141200(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_vsync_callback_enabled())
    {
        const uint32_t postCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t postCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu);    // -0x77A4
        const uint32_t postInCb = dc2_read_u32(rdram, gp + 0xFFFF8860u);  // -0x77A0
        const uint32_t postField = dc2_read_u32(rdram, gp + 0xFFFF87B8u); // -0x7848
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 16u || (n % 120u) == 0u || dtUs > 500)
        {
            std::fprintf(stderr,
                         "[F49:vsynccb] n=%u tick=%u ra=0x%x cb=0x%x->0x%x inCb=%u->%u count=%u->%u field=%u->%u dtUs=%lld\n",
                         n, tickArg, getRegU32(ctx, 31), preCb, postCb,
                         preInCb, postInCb, preCount, postCount, preField, postField,
                         static_cast<long long>(dtUs));
        }
    }
}

static void f49_syncv_callback_set_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t reqCb = getRegU32(ctx, 4);
    const uint32_t preCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu); // -0x77A4
    sceGsSyncVCallback_0x1041e8(rdram, ctx, runtime);

    if (f49_trace_syncv_set_enabled())
    {
        const uint32_t postCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu); // -0x77A4
        const uint32_t retPrev = getRegU32(ctx, 2);
        if (n <= 8u || reqCb == 0u || reqCb != preCb || postCb != preCb)
        {
            std::fprintf(stderr,
                         "[F49:syncvset] n=%u req=0x%x pre=0x%x post=0x%x retPrev=0x%x ra=0x%x\n",
                         n, reqCb, preCb, postCb, retPrev, getRegU32(ctx, 31));
        }
    }
}

static void f49_scegssyncv_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t preCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const auto t0 = std::chrono::steady_clock::now();
    sceGsSyncV_0x103300(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_syncv_call_enabled())
    {
        const uint32_t postCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dv = postCount - preCount;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 32u || dtMs >= 30 || (n % 240u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:syncv] n=%u ra=0x%x pre=%u post=%u dv=%u dtMs=%lld ret=%u\n",
                         n, ra, preCount, postCount, dv,
                         static_cast<long long>(dtMs), getRegU32(ctx, 2));
        }
    }
}
