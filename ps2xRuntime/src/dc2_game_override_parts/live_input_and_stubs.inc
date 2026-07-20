// PHASE G7: poll the live host controller (XInput via raylib) once per host present
// frame and publish the snapshot the guest pad read consumes. Runs on the raylib/main
// thread (called from the ps2_runtime.cpp present loop) where raylib input state is
// valid. Global linkage so ps2_runtime.cpp can call it; reads the anonymous-namespace
// snapshot globals (same-TU visibility, like f66_drive_dungeon_pad). Marker:
// G7_XINPUT_LIVE.
extern "C" bool dc2_poll_host_pad(bool, uint16_t *, uint8_t *, uint8_t *,
                                  uint8_t *, uint8_t *);

// G55: per-present-frame title map draw-state probe (called from the host present loop in
// ps2_runtime.cpp). The TitleMapDraw@0x2a2280 override only fires on INDIRECT dispatch, so it
// cannot sample steady state. PreDraw__4CMap@0x15d7a0 gates the WHOLE draw-list rebuild on a
// map-level box test: it runs only if (map+0x334==0) OR mgInsideScreen(map+0x340). If that map
// box projects off-screen, PreDraw early-outs and the per-frame draw list (map+0x360) stays 0
// -> nothing draws even though every part has a visual + bbox. Gated DC2_TRACE_G53. Global
// linkage (outside the anonymous namespace) so ps2_runtime.cpp can call it.
void g55_title_draw_probe(uint8_t *rdram)
{
    if (!g53_trace_enabled())
        return;
    static uint32_t frames = 0u, logs = 0u;
    const uint32_t f = ++frames;
    if (logs >= 120u || (f % 120u) != 0u)
        return;
    const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
    if (!titleMap)
        return;
    const uint32_t boxEnable = dc2_read_u32(rdram, titleMap + 0x334u);
    const uint32_t drawList = dc2_read_u32(rdram, titleMap + 0x360u);
    const uint32_t mBase = dc2_read_u32(rdram, titleMap + 0x32Cu);
    const uint32_t mCnt = dc2_read_u32(rdram, titleMap + 0x330u);
    auto rf = [&](uint32_t a){ uint32_t u = dc2_read_u32(rdram, a); float v; std::memcpy(&v,&u,4); return v; };
    uint32_t visParts = 0u;
    for (uint32_t i = 0u; i < mCnt && i < 6u; ++i)
        if (dc2_read_u32(rdram, mBase + i * 0x310u + 0x2F8u)) ++visParts;
    // map+0x340 = mgVu0FBOX: +0x00 max.xyzw, +0x10 min.xyzw (G37 convention).
    std::fprintf(stderr,
                 "[G55:titledraw] f=%u TitleMap=0x%x boxEnable(+0x334)=0x%x drawList(+0x360)=%u visParts=%u/%u box.max=(%.1f,%.1f,%.1f) box.min=(%.1f,%.1f,%.1f)\n",
                 f, titleMap, boxEnable, drawList, visParts, mCnt,
                 rf(titleMap + 0x340u), rf(titleMap + 0x344u), rf(titleMap + 0x348u),
                 rf(titleMap + 0x350u), rf(titleMap + 0x354u), rf(titleMap + 0x358u));
    std::fflush(stderr);
    ++logs;
}

void g7_poll_live_pad()
{
    static const bool noXInput  = dc2_env_flag_enabled("DC2_NO_XINPUT");
    static const bool allowKbd  = dc2_env_flag_enabled("DC2_KEYBOARD");
    static const bool explicitS = f40_explicit_script();
    if (noXInput || explicitS)
    {
        g_pad_live_connected = false;
        return;
    }

    uint16_t mask = 0u;
    uint8_t lx = 0x80u, ly = 0x80u, rx = 0x80u, ry = 0x80u;
    const bool connected = dc2_poll_host_pad(allowKbd, &mask, &lx, &ly, &rx, &ry);

    // rawMask = standard active-high scePad bits from dc2_poll_host_pad, BEFORE any
    // byte-swap correction. This is the value the scripted replay path uses (f40_btn_bit
    // maps the same standard scePad bit positions), so the recorder logs rawMask so that
    // replay_from_rec.py ג†’ DC2_PAD_INPUT produces bit-identical replay behaviour.
    const uint16_t rawMask = mask;

    // G7-fix: the live button word reaches the game byte-swapped relative to the
    // documented scePad layout (verified on real play: physical Cross 0x4000 acted as
    // D-pad Down 0x40, Triangle 0x1000 as Up 0x10, R1 0x800 as Start 0x8 ג€” a clean
    // high/low byte swap). Swap the two button bytes so the game reads the intended
    // scePad bits. Only the live path is corrected; the scripted F40/F66 paths (and
    // their trial-tuned aliases / golden smoke) are left untouched. Disable with
    // DC2_PAD_NOSWAP=1 if a controller is found that does not need it.
    static const bool noSwap = dc2_env_flag_enabled("DC2_PAD_NOSWAP");
    if (!noSwap)
        mask = static_cast<uint16_t>(((mask & 0x00FFu) << 8) | ((mask & 0xFF00u) >> 8));

    g_pad_live_mask = mask;
    g_pad_live_lx = lx; g_pad_live_ly = ly;
    g_pad_live_rx = rx; g_pad_live_ry = ry;
    g_pad_live_connected = connected;

    // Input recorder (DC2_REC_INPUT=1): log every live-pad CHANGE with the front-end
    // script-frame (g_f40_frame_counter, the same clock DC2_PAD_INPUT replays against), a
    // wall-clock ms stamp, the standard scePad active-high mask (rawMask, pre-swap), and the
    // four analog axes. Lets a live play session be replayed headless via DC2_PAD_INPUT.
    // File: DC2_REC_INPUT_FILE or default captures/input_rec.txt.
    // Decode bits: Select1 L3-2 R3-4 Start8 Up10 Right20 Down40 Left80
    // L2-100 R2-200 L1-400 R1-800 Tri-1000 Circle-2000 Cross-4000 Square-8000.
    if (connected)
    {
        static const bool recInput = dc2_env_flag_enabled("DC2_REC_INPUT");
        if (recInput)
        {
            static std::FILE *recF = nullptr;
            static auto recT0 = std::chrono::steady_clock::now();
            static uint16_t lastMask = 0xFFFFu;
            static uint8_t lastLx = 0x80u, lastLy = 0x80u, lastRx = 0x80u, lastRy = 0x80u;
            static bool recOpenTried = false;
            if (!recF && !recOpenTried)
            {
                recOpenTried = true;
                const char *path = std::getenv("DC2_REC_INPUT_FILE");
                recF = std::fopen(path ? path : "captures/input_rec.txt", "w");
                if (recF)
                    std::fprintf(recF, "# DC2 live input recording. f=scriptFrame ms=wallclock "
                                       "mask=scePad(active-high) lx ly rx ry. Replay via DC2_PAD_INPUT keyed on f.\n");
            }
            const bool changed = (rawMask != lastMask) || (lx != lastLx) || (ly != lastLy) ||
                                 (rx != lastRx) || (ry != lastRy);
            if (recF && changed)
            {
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - recT0).count();
                std::fprintf(recF, "f=%u ms=%lld mask=0x%04x lx=0x%02x ly=0x%02x rx=0x%02x ry=0x%02x\n",
                             g_f40_frame_counter, ms, static_cast<unsigned>(rawMask), lx, ly, rx, ry);
                std::fflush(recF);
                lastMask = rawMask; lastLx = lx; lastLy = ly; lastRx = rx; lastRy = ry;
            }
        }
    }

    static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
    if (trace && connected)
    {
        static uint32_t c = 0u;
        if ((mask != 0u) || (c++ % 120u) == 0u)
            std::fprintf(stderr,
                "[G7:live] connected=1 mask=0x%04x lx=0x%02x ly=0x%02x rx=0x%02x ry=0x%02x\n",
                static_cast<unsigned>(mask), lx, ly, rx, ry);
    }
}
