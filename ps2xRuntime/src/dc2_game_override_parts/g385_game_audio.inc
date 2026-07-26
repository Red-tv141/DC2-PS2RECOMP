// PHASE G385: focused capture for DC2's EE-side MODMSIN message producers.
// The original functions are presently bound to return-zero auto-stubs, so trace
// the actual arguments at that boundary. Default-off; DC2_G385_AUDIO_TRACE=1.

static bool g385_audio_trace_enabled()
{
    return dc2_env_flag_enabled("DC2_G385_AUDIO_TRACE");
}

static bool g385_game_audio_disabled()
{
    return dc2_env_flag_enabled("DC2_G385_NO_GAME_AUDIO");
}

// G390 rollback. Cached once: this is read per MODMSIN message.
static bool g390_legacy_sfx_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G390_LEGACY_SFX");
    return enabled;
}

struct G385MsinPortState
{
    uint8_t program = 0u;
    uint8_t channelVolume = 127u;
    // G392: `F9 01` is the PAN, not the volume (see g392_legacy_se_mix_enabled).
    uint8_t channelPan = 64u;
    bool channelPanSeen = false;
};

// G392 rollback: restore the G385 reading of the SE parameter stream, in which
// `F9 01` was taken as the channel volume and `F9 00` was ignored.
//
// The real order comes from the guest: `sndSePlaySeID(id, seNo, prio, vol,
// pan = 0x40, pitch = 0x2000, ...)` -> `sndSePlayPrKr` -> `sndSePlayPBPrKr`
// -> `SE_Play__6CSoundFiiiiiiiii`, which emits `F9 00 <vol>`, `F9 01 <pan>`,
// `F9 02 <pitch14>` before the `FD 10` key-on. The recorded route agrees:
// the `F9 01` stream has median 0x40 with 66 of 131 messages exactly 0x40
// (the pan default), while `F9 00` spans 0..0x48 and never hits 0x40.
static bool g392_legacy_se_mix_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G392_LEGACY_SE_MIX");
    return enabled;
}

static std::array<G385MsinPortState, 16u> g_g385MsinPorts{};

static void g385_stream_open_trace(uint8_t *rdram, R5900Context *ctx,
                                   PS2Runtime *runtime)
{
    const uint32_t port = getRegU32(ctx, 5);
    const uint32_t pathAddr = getRegU32(ctx, 6);
    const char *path = pathAddr
        ? reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr))
        : nullptr;
    uint32_t pathLength = 0u;
    while (path != nullptr && pathLength < 160u && path[pathLength] != '\0')
    {
        ++pathLength;
    }
    std::fprintf(stderr,
                 "[G385:voice] open port=%u pathAddr=0x%08x path=\"%.*s\" ra=0x%08x\n",
                 port, pathAddr, static_cast<int>(pathLength),
                 path != nullptr ? path : "", getRegU32(ctx, 31));

    ::StreamOpenFast__6CSoundFiPc_0x18aef0(rdram, ctx, runtime);
}

static void g385_trace_msin_message(const char *kind, uint8_t *rdram,
                                    R5900Context *ctx, const uint8_t *bytes,
                                    uint32_t byteCount)
{
    static uint32_t traceCount = 0u;
    if (!g385_audio_trace_enabled() || traceCount >= 1024u)
    {
        return;
    }

    std::fprintf(stderr,
                 "[G385:msin] n=%u kind=%s object=0x%08x port=0x%08x msg=",
                 traceCount++, kind, getRegU32(ctx, 4), getRegU32(ctx, 5));
    for (uint32_t i = 0u; bytes != nullptr && i < byteCount; ++i)
    {
        std::fprintf(stderr, "%02x", static_cast<unsigned>(bytes[i]));
    }
    std::fprintf(stderr, " ra=0x%08x\n", getRegU32(ctx, 31));
}

static void g385_msin_putmsg_trace(uint8_t *rdram, R5900Context *ctx,
                                   PS2Runtime *runtime)
{
    (void)runtime;
    const uint32_t packed = getRegU32(ctx, 6);
    const uint8_t status = static_cast<uint8_t>(packed & 0xFFu);
    const uint32_t port = getRegU32(ctx, 5) & 0x0Fu;
    if ((status & 0xF0u) == 0xC0u)
    {
        g_g385MsinPorts[port].program = static_cast<uint8_t>((packed >> 8u) & 0x7Fu);
    }
    else if ((status & 0xF0u) == 0xB0u)
    {
        const uint8_t controller = static_cast<uint8_t>((packed >> 8u) & 0x7Fu);
        if (controller == 7u)
        {
            g_g385MsinPorts[port].channelVolume =
                static_cast<uint8_t>((packed >> 16u) & 0x7Fu);
        }
    }
    const uint32_t byteCount =
        ((status & 0xF0u) == 0xC0u || (status & 0xF0u) == 0xD0u) ? 2u : 3u;
    uint8_t bytes[4]{};
    std::memcpy(bytes, &packed, sizeof(packed));
    g385_trace_msin_message("midi", rdram, ctx, bytes, byteCount);
    setReturnS32(ctx, 0);
}

static void g385_msin_puthsmsg_trace(uint8_t *rdram, R5900Context *ctx,
                                     PS2Runtime *runtime)
{
    (void)runtime;
    const uint32_t msgAddr = getRegU32(ctx, 6);
    const uint8_t *bytes = msgAddr ? getConstMemPtr(rdram, msgAddr) : nullptr;
    uint32_t byteCount = 0u;
    if (bytes != nullptr)
    {
        byteCount = (bytes[0] == 0xF9u) ? 5u : ((bytes[0] == 0xFDu) ? 7u : 1u);
    }
    g385_trace_msin_message("hs", rdram, ctx, bytes, byteCount);
    const uint32_t port = getRegU32(ctx, 5) & 0x0Fu;
    if (!g385_game_audio_disabled() && bytes != nullptr)
    {
        if (bytes[0] == 0xF9u && byteCount >= 5u && bytes[1] <= 1u)
        {
            const uint8_t value = bytes[3] & 0x7Fu;
            if (g392_legacy_se_mix_enabled())
            {
                if (bytes[1] == 1u)
                {
                    g_g385MsinPorts[port].channelVolume = value;
                }
            }
            else if (bytes[1] == 0u)
            {
                g_g385MsinPorts[port].channelVolume = value;
            }
            else
            {
                g_g385MsinPorts[port].channelPan = value;
                g_g385MsinPorts[port].channelPanSeen = true;
            }
        }
        else if (bytes[0] == 0xFDu && byteCount >= 7u)
        {
            // The 7-byte MODMSIN hs message is `FD <sub> 00 <key> <id> <value>
            // <extra>`: sub 0x10 = SE_Play/SE_Stop, 0x00 = SE_SetVol,
            // 0x01 = SE_SetPan, 0x02 = SE_SetPitch (14-bit, value|extra<<7).
            const uint8_t key = bytes[3] & 0x7Fu;
            const uint8_t voiceId = bytes[4] & 0x7Fu;
            const uint8_t value = bytes[5] & 0x7Fu;
            if (bytes[1] == 0x10u)
            {
                // G390: MIDI semantics — a key-on with velocity 0 IS a key-off
                // (that is exactly what `SE_Stop__6CSoundFiiiii` emits).
                // G391 releases the matching voice instead of dropping it.
                if (value != 0u || g390_legacy_sfx_enabled())
                {
                    ::dc2G385PlaySfx(&runtime->audioBackend(), port + 7u,
                                     g_g385MsinPorts[port].program, key, value,
                                     g_g385MsinPorts[port].channelVolume, voiceId,
                                     g_g385MsinPorts[port].channelPanSeen
                                         ? g_g385MsinPorts[port].channelPan
                                         : 0xFFu);
                }
                else
                {
                    ::dc2G385StopSfx(port + 7u, key, voiceId);
                }
            }
            else if (bytes[1] <= 0x02u)
            {
                const uint32_t wide =
                    bytes[1] == 0x02u
                        ? (static_cast<uint32_t>(value) |
                           (static_cast<uint32_t>(bytes[6] & 0x7Fu) << 7u))
                        : static_cast<uint32_t>(value);
                ::dc2G385SetSfxParam(port + 7u, key, voiceId, bytes[1], wide);
            }
        }
    }
    setReturnS32(ctx, 0);
}

static void applyDC2G385AudioTrace(PS2Runtime &runtime)
{
    if (g385_game_audio_disabled() && !g385_audio_trace_enabled())
    {
        return;
    }

    if (dc2_env_flag_enabled("DC2_G391_SELFTEST"))
    {
        ::dc2G391SelfTest();
    }

    runtime.registerFunction(0x00123D38u, g385_msin_putmsg_trace);
    runtime.registerFunction(0x00123E60u, g385_msin_puthsmsg_trace);
    if (g385_audio_trace_enabled())
    {
        runtime.registerFunction(0x0018AEF0u, g385_stream_open_trace);
    }
}
