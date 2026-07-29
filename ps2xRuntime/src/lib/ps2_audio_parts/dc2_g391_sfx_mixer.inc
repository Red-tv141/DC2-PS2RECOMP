// PHASE G391 — DC2 bank-SFX voice mixer.
//
// Before G391 a MODMSIN key-on was turned into a fire-and-forget raylib `Sound`:
// no envelope (the SPU2 ADSR in the Sony `.HD` was never parsed), no VAG loop
// points (a sustained one-shot stopped at sample end), no pan (`play()` takes
// pitch+volume only), no key-off (G390 correctly stopped *playing* velocity-0
// messages, but nothing consumed them either) and no reverb.
//
// This file renders those one-shots itself: one 44.1 kHz stereo `AudioStream`
// fed by up to 24 voices (the SPU2 per-core voice count), each with a real SPU2
// ADSR envelope, loop-aware sample playback, equal-power pan and a shared
// reverb send driven by the guest's own `SetReverb__6CSoundFiii` parameters.
//
// Same-binary rollback: `DC2_G391_LEGACY_SFX_PATH=1` restores the G390
// `PS2AudioBackend::play()` one-shot path (and with it the G390 key-off drop).

namespace
{
    constexpr uint32_t kG391OutputRate = 44100u;
    constexpr size_t kG391MaxVoices = 24u;
    constexpr int32_t kG391EnvMax = 0x7FFF;

    bool g391LegacySfxPathEnabled()
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("DC2_G391_LEGACY_SFX_PATH");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return enabled;
    }

    bool g391ReverbDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G391_NO_REVERB");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    bool g391AdsrDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G391_NO_ADSR");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    // G394 rollback: apply an early post-start volume-zero/stop pair literally.
    // DC2 can flush an older sndSqStop after its replacement Play command.
    bool g394AsyncStopGuardDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G394_LEGACY_ASYNC_STOP");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    // G391 rollback: restore the pre-G391 per-sequence peak normalisation.
    bool g391LegacyBgmNormaliseEnabled()
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("DC2_G391_LEGACY_BGM_NORM");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return enabled;
    }

    // One fixed gain for every rendered sequence: relative loudness between
    // tracks is preserved, and the occasional overshoot is rounded off by the
    // soft knee below instead of pulling the whole track down.
    constexpr float kG391SequenceBusGain = 0.85f;

    float g391SoftLimit(float value)
    {
        constexpr float kKnee = 0.75f;
        const float magnitude = std::fabs(value);
        if (magnitude <= kKnee)
        {
            return value;
        }
        const float over = (magnitude - kKnee) / (1.0f - kKnee);
        const float limited = kKnee + (1.0f - kKnee) * std::tanh(over);
        return value < 0.0f ? -limited : limited;
    }

    // ---------------------------------------------------------------- ADSR --
    //
    // SPU2 envelope, level 0..0x7FFF, one tick per output sample (the envelope
    // clock IS the 44.1 kHz sample clock on hardware).
    //
    //   ADSR1: bit15 Am (attack mode, 1 = exponential), bits8-14 Ar,
    //          bits4-7 Dr, bits0-3 Sl (sustain level = (Sl+1) * 0x800)
    //   ADSR2: bit15 Sm, bit14 Sd (1 = decreasing), bits6-12 Sr,
    //          bit5 Rm, bits0-4 Rr
    //
    // Rate/decay rates are 4-bit or 5-bit fields scaled by 4 to reach the same
    // 0..127 rate domain as Ar/Sr.
    struct G391Adsr
    {
        uint16_t adsr1 = 0x80FFu; // instant attack, no decay, full sustain
        uint16_t adsr2 = 0x5FC0u; // hold, moderate exponential release
    };

    struct G391RateStep
    {
        int32_t step = 0;
        uint32_t counterIncrement = 0u;
    };

    G391RateStep g391RateStep(uint32_t rate, bool exponential, bool increase,
                              int32_t level)
    {
        G391RateStep out{};
        out.step = increase ? (7 - static_cast<int32_t>(rate & 3u))
                            : (static_cast<int32_t>(rate & 3u) - 8);
        const uint32_t shift = rate >> 2u;
        if (shift < 11u)
        {
            out.step <<= (11u - shift);
            out.counterIncrement = 0x8000u;
        }
        else
        {
            const uint32_t excess = shift - 11u;
            out.counterIncrement = excess >= 31u ? 0u : (0x8000u >> excess);
        }
        if (exponential)
        {
            if (increase)
            {
                if (level > 0x6000)
                {
                    out.counterIncrement >>= 2u;
                }
            }
            else
            {
                out.step = static_cast<int32_t>(
                    (static_cast<int64_t>(out.step) * static_cast<int64_t>(level)) >> 15);
            }
        }
        return out;
    }

    struct G391EnvState
    {
        enum class Phase : uint8_t
        {
            Attack,
            Decay,
            Sustain,
            Release,
            Off,
        };

        Phase phase = Phase::Attack;
        int32_t level = 0;
        uint32_t counter = 0u;

        void keyOff()
        {
            if (phase != Phase::Off)
            {
                phase = Phase::Release;
            }
        }

        bool finished() const { return phase == Phase::Off; }

        // Returns the current envelope level (0..0x7FFF) and advances one tick.
        int32_t tick(const G391Adsr &adsr)
        {
            const int32_t current = level;
            if (phase == Phase::Off)
            {
                return 0;
            }

            uint32_t rate = 0u;
            bool exponential = false;
            bool increase = false;
            switch (phase)
            {
            case Phase::Attack:
                rate = (adsr.adsr1 >> 8u) & 0x7Fu;
                exponential = (adsr.adsr1 & 0x8000u) != 0u;
                increase = true;
                break;
            case Phase::Decay:
                rate = ((adsr.adsr1 >> 4u) & 0x0Fu) * 4u;
                exponential = true;
                increase = false;
                break;
            case Phase::Sustain:
                rate = (adsr.adsr2 >> 6u) & 0x7Fu;
                exponential = (adsr.adsr2 & 0x8000u) != 0u;
                increase = (adsr.adsr2 & 0x4000u) == 0u;
                break;
            case Phase::Release:
                rate = (adsr.adsr2 & 0x1Fu) * 4u;
                exponential = (adsr.adsr2 & 0x0020u) != 0u;
                increase = false;
                break;
            case Phase::Off:
                return 0;
            }

            const G391RateStep step = g391RateStep(rate, exponential, increase, level);
            counter += step.counterIncrement;
            if (counter >= 0x8000u)
            {
                counter -= 0x8000u;
                level = std::max<int32_t>(0, std::min<int32_t>(kG391EnvMax,
                                                               level + step.step));
            }

            switch (phase)
            {
            case Phase::Attack:
                if (level >= kG391EnvMax)
                {
                    phase = Phase::Decay;
                }
                break;
            case Phase::Decay:
            {
                const int32_t sustainLevel =
                    std::min<int32_t>(kG391EnvMax,
                                      (static_cast<int32_t>(adsr.adsr1 & 0x0Fu) + 1) * 0x800);
                if (level <= sustainLevel)
                {
                    phase = Phase::Sustain;
                }
                break;
            }
            case Phase::Release:
                if (level <= 0)
                {
                    phase = Phase::Off;
                }
                break;
            default:
                break;
            }
            return current;
        }
    };

    // ------------------------------------------------------------- decoding --

    // G391: the loop-flag scan used to live inside `g385GetRenderInstrument`.
    // One-shots need exactly the same information (item: "bank SFX ignore VAG
    // loop points"), so it is shared from here.
    std::shared_ptr<G385DecodedVag> g391DecodeVagWithLoops(
        const G385ResolvedSample &sample)
    {
        const std::vector<uint8_t> vagp = g385BuildVagp(sample);
        auto decoded = std::make_shared<G385DecodedVag>();
        if (!ps2_vag::decode(vagp.data(), static_cast<uint32_t>(vagp.size()),
                             decoded->pcm, decoded->sampleRate) ||
            decoded->pcm.empty())
        {
            return nullptr;
        }

        decoded->playEnd = static_cast<uint32_t>(decoded->pcm.size());
        uint32_t loopStart = 0u;
        bool sawLoopStart = false;
        for (size_t blockOffset = 0u; blockOffset + 16u <= sample.vag.size();
             blockOffset += 16u)
        {
            const uint8_t flags = sample.vag[blockOffset + 1u];
            const uint32_t pcmFrame = static_cast<uint32_t>((blockOffset / 16u) * 28u);
            if ((flags & 0x04u) != 0u && !sawLoopStart)
            {
                loopStart = pcmFrame;
                sawLoopStart = true;
            }
            if ((flags & 0x01u) != 0u)
            {
                decoded->playEnd = std::min<uint32_t>(decoded->playEnd, pcmFrame + 28u);
                decoded->loops = sawLoopStart && (flags & 0x02u) != 0u &&
                                 loopStart < decoded->playEnd;
                break;
            }
        }
        decoded->loopStart = decoded->loops ? loopStart : 0u;
        return decoded;
    }

    // -------------------------------------------------------------- reverb --
    //
    // G392: the real SPU reverb network, not a generic Schroeder box.
    // G393: the register sets below now come from DC2's retail LIBSD.IRX
    // (.data file offsets 0x4278..0x44d7), including its PS2-only PIPE mode.
    //
    // The guest configures reverb through `SetReverb__6CSoundFiii@0x188940`
    // (`sceSdRemote` 0x8130 SetEffectAttr + core|2 enable + the 0xb80/0xc80 EVOL
    // writes), so mode and wet level are the guest's own values. What the port
    // G392 originally used documented PSX/SPU presets. G393 recovered the exact
    // PS2 libsd values statically from the retail IRX, avoiding an architectural
    // dependency on executing the IOP merely to observe this immutable table.
    //
    // Per psx-spx the network runs at 22050 Hz over a delay work area, and every
    // m*/d* register is an address in 8-byte units => four 16-bit samples.
    struct G392ReverbPreset
    {
        const char *name;
        uint32_t spuBytes;
        uint16_t reg[32];
    };

    // Register order: dAPF1 dAPF2 vIIR vCOMB1 vCOMB2 vCOMB3 vCOMB4 vWALL vAPF1
    // vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME mLDIFF
    // mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2
    // mRAPF2 vLIN vRIN
    enum G392ReverbField
    {
        kDAPF1 = 0, kDAPF2, kVIIR, kVCOMB1, kVCOMB2, kVCOMB3, kVCOMB4, kVWALL,
        kVAPF1, kVAPF2, kMLSAME, kMRSAME, kMLCOMB1, kMRCOMB1, kMLCOMB2, kMRCOMB2,
        kDLSAME, kDRSAME, kMLDIFF, kMRDIFF, kMLCOMB3, kMRCOMB3, kMLCOMB4,
        kMRCOMB4, kDLDIFF, kDRDIFF, kMLAPF1, kMRAPF1, kMLAPF2, kMRAPF2, kVLIN,
        kVRIN,
    };

    const G392ReverbPreset kG392ReverbPresets[] = {
        {"Off", 0x10, {
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
            0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
            0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000}},
        {"Room", 0x26C0, {
            0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
            0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
            0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5,
            0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000}},
        {"Studio Small", 0x1F40, {
            0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00,
            0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
            0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
            0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000}},
        {"Studio Medium", 0x4840, {
            0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
            0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
            0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
            0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000}},
        {"Studio Large", 0x6FE0, {
            0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
            0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
            0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
            0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000}},
        {"Hall", 0xADE0, {
            0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
            0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
            0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
            0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000}},
        {"Pipe", 0x3C00, {
            0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500,
            0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
            0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
            0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000}},
        {"Space Echo", 0xF6C0, {
            0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000,
            0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
            0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
            0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000}},
        {"Echo", 0x18040, {
            0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100,
            0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
            0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
            0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000}},
        {"Delay", 0x18040, {
            0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
            0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
            0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
            0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000}},
    };

    // libsd SD_REV_MODE_* -> retail LIBSD.IRX table index.
    size_t g392PresetForMode(uint32_t mode)
    {
        switch (mode)
        {
        case 1u: return 1u; // ROOM
        case 2u: return 2u; // STUDIO_A -> Studio Small
        case 3u: return 3u; // STUDIO_B -> Studio Medium
        case 4u: return 4u; // STUDIO_C -> Studio Large
        case 5u: return 5u; // HALL
        case 6u: return 7u; // SPACE    -> Space Echo
        case 7u: return 8u; // ECHO
        case 8u: return 9u; // DELAY
        case 9u: return 6u; // PIPE
        default: return 0u; // OFF
        }
    }

    struct G391Reverb
    {
        std::vector<float> buffer; // work area, one float per 22050 Hz sample
        size_t cursor = 0u;
        size_t bufferSamples = 0u;
        uint32_t mode = 0u;
        size_t presetIndex = 0u;
        float wet = 0.0f;
        bool configured = false;
        // Half-rate state: the network ticks on every other 44.1 kHz frame,
        // exactly like the hardware.
        bool tickPhase = false;
        float heldL = 0.0f;
        float heldR = 0.0f;
        float pendingL = 0.0f;
        float pendingR = 0.0f;

        const G392ReverbPreset &preset() const
        {
            return kG392ReverbPresets[presetIndex];
        }

        static float coef(uint16_t value)
        {
            return static_cast<float>(static_cast<int16_t>(value)) / 32768.0f;
        }

        // Register addresses are in 8-byte units over 16-bit samples, so one
        // register step is four samples.
        size_t off(G392ReverbField field) const
        {
            const size_t samples = static_cast<size_t>(preset().reg[field]) * 4u;
            return bufferSamples != 0u ? (samples % bufferSamples) : 0u;
        }

        float &at(size_t offset)
        {
            return buffer[(cursor + offset) % bufferSamples];
        }

        void configure(uint32_t newMode, float newWet)
        {
            const size_t newPreset = g392PresetForMode(newMode);
            if (!configured || newPreset != presetIndex)
            {
                presetIndex = newPreset;
                bufferSamples =
                    std::max<size_t>(8u, kG392ReverbPresets[presetIndex].spuBytes / 2u);
                buffer.assign(bufferSamples, 0.0f);
                cursor = 0u;
                heldL = 0.0f;
                heldR = 0.0f;
                pendingL = 0.0f;
                pendingR = 0.0f;
                configured = true;
            }
            mode = newMode;
            wet = newWet;
        }

        bool active() const
        {
            return configured && mode != 0u && presetIndex != 0u && wet > 0.0f;
        }

        void process(float inL, float inR, float &outL, float &outR)
        {
            if (!active())
            {
                outL = 0.0f;
                outR = 0.0f;
                return;
            }

            // Both 44.1 kHz frames feed the 22050 Hz input; tick on the second
            // one and hold the result across the pair.
            pendingL += inL * 0.5f;
            pendingR += inR * 0.5f;
            tickPhase = !tickPhase;
            if (tickPhase)
            {
                outL = heldL;
                outR = heldR;
                return;
            }

            const G392ReverbPreset &p = preset();
            const float lin = pendingL * coef(p.reg[kVLIN]);
            const float rin = pendingR * coef(p.reg[kVRIN]);
            pendingL = 0.0f;
            pendingR = 0.0f;

            const float vIIR = coef(p.reg[kVIIR]);
            const float vWALL = coef(p.reg[kVWALL]);

            // Same-side reflection (left-to-left, right-to-right).
            const size_t mLSAME = off(kMLSAME);
            const size_t mRSAME = off(kMRSAME);
            const float lSamePrev = at(mLSAME + bufferSamples - 4u);
            const float rSamePrev = at(mRSAME + bufferSamples - 4u);
            at(mLSAME) = (lin + at(off(kDLSAME)) * vWALL - lSamePrev) * vIIR + lSamePrev;
            at(mRSAME) = (rin + at(off(kDRSAME)) * vWALL - rSamePrev) * vIIR + rSamePrev;

            // Different-side reflection (left-to-right, right-to-left).
            const size_t mLDIFF = off(kMLDIFF);
            const size_t mRDIFF = off(kMRDIFF);
            const float lDiffPrev = at(mLDIFF + bufferSamples - 4u);
            const float rDiffPrev = at(mRDIFF + bufferSamples - 4u);
            at(mLDIFF) = (lin + at(off(kDRDIFF)) * vWALL - lDiffPrev) * vIIR + lDiffPrev;
            at(mRDIFF) = (rin + at(off(kDLDIFF)) * vWALL - rDiffPrev) * vIIR + rDiffPrev;

            // Early echo (comb filter, input from the work area).
            float lout = coef(p.reg[kVCOMB1]) * at(off(kMLCOMB1)) +
                         coef(p.reg[kVCOMB2]) * at(off(kMLCOMB2)) +
                         coef(p.reg[kVCOMB3]) * at(off(kMLCOMB3)) +
                         coef(p.reg[kVCOMB4]) * at(off(kMLCOMB4));
            float rout = coef(p.reg[kVCOMB1]) * at(off(kMRCOMB1)) +
                         coef(p.reg[kVCOMB2]) * at(off(kMRCOMB2)) +
                         coef(p.reg[kVCOMB3]) * at(off(kMRCOMB3)) +
                         coef(p.reg[kVCOMB4]) * at(off(kMRCOMB4));

            // Late reverb all-pass 1, then all-pass 2.
            const float vAPF1 = coef(p.reg[kVAPF1]);
            const size_t mLAPF1 = off(kMLAPF1);
            const size_t mRAPF1 = off(kMRAPF1);
            const size_t dAPF1 =
                (static_cast<size_t>(p.reg[kDAPF1]) * 4u) % bufferSamples;
            const float lApf1Tap = at(mLAPF1 + bufferSamples - dAPF1);
            const float rApf1Tap = at(mRAPF1 + bufferSamples - dAPF1);
            lout = lout - vAPF1 * lApf1Tap;
            rout = rout - vAPF1 * rApf1Tap;
            at(mLAPF1) = lout;
            at(mRAPF1) = rout;
            lout = lout * vAPF1 + lApf1Tap;
            rout = rout * vAPF1 + rApf1Tap;

            const float vAPF2 = coef(p.reg[kVAPF2]);
            const size_t mLAPF2 = off(kMLAPF2);
            const size_t mRAPF2 = off(kMRAPF2);
            const size_t dAPF2 =
                (static_cast<size_t>(p.reg[kDAPF2]) * 4u) % bufferSamples;
            const float lApf2Tap = at(mLAPF2 + bufferSamples - dAPF2);
            const float rApf2Tap = at(mRAPF2 + bufferSamples - dAPF2);
            lout = lout - vAPF2 * lApf2Tap;
            rout = rout - vAPF2 * rApf2Tap;
            at(mLAPF2) = lout;
            at(mRAPF2) = rout;
            lout = lout * vAPF2 + lApf2Tap;
            rout = rout * vAPF2 + rApf2Tap;

            // The work area advances one 22050 Hz sample per tick.
            cursor = (cursor + 1u) % bufferSamples;

            heldL = lout;
            heldR = rout;
            outL = lout;
            outR = rout;
        }
    };

    // -------------------------------------------------------------- voices --

    struct G391Voice
    {
        std::shared_ptr<G385DecodedVag> decoded;
        G391Adsr adsr{};
        G391EnvState env{};
        double position = 0.0;
        double step = 1.0;
        float gain = 1.0f;
        float baseGain = 1.0f; // gain with the key-on channel volume divided out
        float panLeft = 0.70710678f;
        float panRight = 0.70710678f;
        uint64_t serial = 0u;
        uint8_t slot = 0u;
        uint8_t key = 0u;
        uint8_t id = 0u;
        bool active = false;
        bool released = false;
    };

    // G392: sequenced BGM used to own a private raylib AudioStream per port, so
    // it bypassed the mixer entirely and could take no reverb send — on hardware
    // the BGM voices feed the same effect bus as the SFX voices. The rendered
    // sequence is already 44.1 kHz stereo (the mixer rate), so it is mixed here
    // instead. Kill `DC2_G392_NO_BGM_BUS=1` to restore the per-port streams.
    struct G392BgmSource
    {
        std::shared_ptr<std::vector<int16_t>> pcm;
        uint64_t cursorFrame = 0u;
        uint64_t loopStartFrame = 0u;
        uint64_t loopEndFrame = 0u;
        float volume = 1.0f;
        bool active = false;
        bool earlyMutePending = false;
    };

    bool g392BgmBusDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G392_NO_BGM_BUS");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    struct G391SfxMixer
    {
        std::mutex mutex;
        AudioStream stream{};
        bool loaded = false;
        std::array<G391Voice, kG391MaxVoices> voices{};
        std::array<G392BgmSource, 16u> bgm{};
        uint64_t serial = 0u;
        G391Reverb reverb{};
        // Statistics for the phase oracle (read under `mutex`).
        uint64_t keyOns = 0u;
        uint64_t keyOffs = 0u;
        uint64_t steals = 0u;
        uint64_t loopedVoices = 0u;
        uint64_t callbacks = 0u;
        uint32_t tracedActiveBgm = 0u;
    };

    G391SfxMixer g_g391Mixer;

    void g391SetPan(G391Voice &voice, float pan)
    {
        const float clamped = std::max(0.0f, std::min(1.0f, pan));
        const double angle = static_cast<double>(clamped) * 1.57079632679489661923;
        voice.panLeft = static_cast<float>(std::cos(angle));
        voice.panRight = static_cast<float>(std::sin(angle));
    }

    // Verification aid: `DC2_G391_MIX_DUMP=<path>` writes the mixer's own output
    // as raw 44.1 kHz stereo s16le, so the voice path can be measured offline
    // instead of trusted from counters.
    std::FILE *g391MixDumpFile()
    {
        static std::FILE *file = []() -> std::FILE *
        {
            const char *path = std::getenv("DC2_G391_MIX_DUMP");
            if (path == nullptr || path[0] == '\0')
            {
                return nullptr;
            }
            return std::fopen(path, "wb");
        }();
        return file;
    }

    void g391MixerCallback(void *bufferData, unsigned int frames)
    {
        int16_t *output = static_cast<int16_t *>(bufferData);
        if (output == nullptr)
        {
            return;
        }

        G391SfxMixer &mixer = g_g391Mixer;
        std::lock_guard<std::mutex> lock(mixer.mutex);
        const bool reverbOn = mixer.reverb.active();
        ++mixer.callbacks;
        uint32_t activeBgm = 0u;
        for (const G392BgmSource &source : mixer.bgm)
        {
            activeBgm += source.active ? 1u : 0u;
        }
        if (g385GameAudioTrace() && activeBgm != mixer.tracedActiveBgm)
        {
            std::fprintf(stderr,
                         "[G394:mix] callback=%llu frames=%u active-bgm=%u\n",
                         static_cast<unsigned long long>(mixer.callbacks), frames,
                         activeBgm);
            mixer.tracedActiveBgm = activeBgm;
        }

        for (unsigned int frame = 0u; frame < frames; ++frame)
        {
            float left = 0.0f;
            float right = 0.0f;

            for (G391Voice &voice : mixer.voices)
            {
                if (!voice.active || !voice.decoded)
                {
                    continue;
                }
                const G385DecodedVag &decoded = *voice.decoded;
                if (decoded.pcm.empty() || decoded.playEnd == 0u)
                {
                    voice.active = false;
                    continue;
                }

                if (voice.position >= static_cast<double>(decoded.playEnd))
                {
                    if (decoded.loops)
                    {
                        const double loopLength =
                            static_cast<double>(decoded.playEnd - decoded.loopStart);
                        voice.position = static_cast<double>(decoded.loopStart) +
                                         std::fmod(voice.position -
                                                       static_cast<double>(decoded.loopStart),
                                                   loopLength);
                    }
                    else
                    {
                        voice.active = false;
                        continue;
                    }
                }

                const uint32_t index = static_cast<uint32_t>(voice.position);
                uint32_t nextIndex = index + 1u;
                if (nextIndex >= decoded.playEnd)
                {
                    nextIndex = decoded.loops ? decoded.loopStart : index;
                }
                if (index >= decoded.pcm.size() || nextIndex >= decoded.pcm.size())
                {
                    voice.active = false;
                    continue;
                }

                const double fraction = voice.position - static_cast<double>(index);
                const float sample =
                    static_cast<float>(static_cast<double>(decoded.pcm[index]) +
                                       (static_cast<double>(decoded.pcm[nextIndex]) -
                                        static_cast<double>(decoded.pcm[index])) *
                                           fraction) /
                    32768.0f;

                const int32_t envLevel = g391AdsrDisabled()
                                             ? kG391EnvMax
                                             : voice.env.tick(voice.adsr);
                const float envGain =
                    static_cast<float>(envLevel) / static_cast<float>(kG391EnvMax);
                const float value = sample * voice.gain * envGain;
                left += value * voice.panLeft;
                right += value * voice.panRight;

                voice.position += voice.step;
                if (!g391AdsrDisabled() && voice.env.finished())
                {
                    voice.active = false;
                }
            }

            // G392: sequenced BGM on the same bus, so it feeds the same effect
            // send the hardware would give it.
            for (G392BgmSource &source : mixer.bgm)
            {
                if (!source.active || !source.pcm ||
                    source.loopEndFrame <= source.loopStartFrame)
                {
                    continue;
                }
                if (source.cursorFrame >= source.loopEndFrame)
                {
                    source.cursorFrame = source.loopStartFrame;
                }
                constexpr uint64_t kG394EarlyStopFrames =
                    kG391OutputRate / 10u;
                if (source.earlyMutePending &&
                    source.cursorFrame >= kG394EarlyStopFrames)
                {
                    source.volume = 0.0f;
                    source.earlyMutePending = false;
                }
                const size_t sampleIndex = static_cast<size_t>(source.cursorFrame) * 2u;
                if (sampleIndex + 1u >= source.pcm->size())
                {
                    source.active = false;
                    continue;
                }
                left += static_cast<float>((*source.pcm)[sampleIndex]) / 32768.0f *
                        source.volume;
                right += static_cast<float>((*source.pcm)[sampleIndex + 1u]) / 32768.0f *
                         source.volume;
                ++source.cursorFrame;
            }

            if (reverbOn)
            {
                float wetL = 0.0f;
                float wetR = 0.0f;
                mixer.reverb.process(left, right, wetL, wetR);
                left += wetL * mixer.reverb.wet;
                right += wetR * mixer.reverb.wet;
            }

            output[frame * 2u + 0u] =
                static_cast<int16_t>(std::lround(g391SoftLimit(left) * 32767.0f));
            output[frame * 2u + 1u] =
                static_cast<int16_t>(std::lround(g391SoftLimit(right) * 32767.0f));
        }

        if (std::FILE *dump = g391MixDumpFile())
        {
            std::fwrite(output, sizeof(int16_t), static_cast<size_t>(frames) * 2u, dump);
            std::fflush(dump);
        }
    }

    // Must be called with `g_g391Mixer.mutex` NOT held.
    bool g391EnsureStream()
    {
#if defined(PLATFORM_VITA)
        return false;
#else
        if (!IsAudioDeviceReady())
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
            if (g_g391Mixer.loaded)
            {
                return true;
            }
        }

        AudioStream stream = LoadAudioStream(kG391OutputRate, 16u, 2u);
        if (!IsAudioStreamValid(stream))
        {
            return false;
        }
        SetAudioStreamCallback(stream, g391MixerCallback);

        bool started = false;
        {
            std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
            if (!g_g391Mixer.loaded)
            {
                g_g391Mixer.stream = stream;
                g_g391Mixer.loaded = true;
                started = true;
            }
        }
        if (!started)
        {
            UnloadAudioStream(stream);
            return true;
        }
        SetAudioStreamVolume(stream, 1.0f);
        PlayAudioStream(stream);
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr, "[G391:mix] stream-start rate=%u voices=%zu\n",
                         kG391OutputRate, kG391MaxVoices);
        }
        return true;
#endif
    }

    // Voice stealing: prefer a free slot, then the oldest released voice, then
    // the oldest voice overall (SPU2 has no priority field here).
    G391Voice *g391AllocateVoice()
    {
        G391Voice *oldestReleased = nullptr;
        G391Voice *oldest = nullptr;
        for (G391Voice &voice : g_g391Mixer.voices)
        {
            if (!voice.active)
            {
                return &voice;
            }
            if (voice.released &&
                (oldestReleased == nullptr || voice.serial < oldestReleased->serial))
            {
                oldestReleased = &voice;
            }
            if (oldest == nullptr || voice.serial < oldest->serial)
            {
                oldest = &voice;
            }
        }
        ++g_g391Mixer.steals;
        return oldestReleased != nullptr ? oldestReleased : oldest;
    }
}

// Key-on. `pan` is 0..1 (0.5 = centre) and `volume` is the already-computed
// G389 mix gain for this one-shot.
void dc2G391KeyOn(uint32_t slot, uint8_t key, uint8_t id,
                  const G385ResolvedSample &sample, float pitch, float volume,
                  float pan, float baseGain)
{
    if (!g391EnsureStream())
    {
        return;
    }

    std::shared_ptr<G385DecodedVag> decoded = g391DecodeVagWithLoops(sample);
    if (!decoded)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
        G391Voice *voice = g391AllocateVoice();
        if (voice == nullptr)
        {
            return;
        }

        *voice = G391Voice{};
        voice->decoded = std::move(decoded);
        voice->adsr.adsr1 = sample.adsr1;
        voice->adsr.adsr2 = sample.adsr2;
        voice->position = 0.0;
        voice->step = static_cast<double>(voice->decoded->sampleRate) *
                      static_cast<double>(pitch) / static_cast<double>(kG391OutputRate);
        voice->gain = volume;
        voice->baseGain = baseGain;
        g391SetPan(*voice, pan);
        voice->serial = ++g_g391Mixer.serial;
        voice->slot = static_cast<uint8_t>(slot);
        voice->key = key;
        voice->id = id;
        voice->active = true;
        voice->released = false;
        ++g_g391Mixer.keyOns;
        if (voice->decoded->loops)
        {
            ++g_g391Mixer.loopedVoices;
        }
        if (g385GameAudioTrace() && (g_g391Mixer.keyOns % 32u) == 0u)
        {
            size_t sounding = 0u;
            for (const G391Voice &active : g_g391Mixer.voices)
            {
                sounding += active.active ? 1u : 0u;
            }
            std::fprintf(stderr,
                         "[G391:mix] keyOns=%llu keyOffs=%llu steals=%llu looped=%llu "
                         "sounding=%zu\n",
                         static_cast<unsigned long long>(g_g391Mixer.keyOns),
                         static_cast<unsigned long long>(g_g391Mixer.keyOffs),
                         static_cast<unsigned long long>(g_g391Mixer.steals),
                         static_cast<unsigned long long>(g_g391Mixer.loopedVoices),
                         sounding);
        }
    }
}

// Key-off (a MODMSIN `FD 10` with velocity 0, or `SE_Stop`). Releases the
// newest matching voice so a retrigger of the same key keeps its own tail.
void dc2G391KeyOff(uint32_t slot, uint8_t key, uint8_t id)
{
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    G391Voice *best = nullptr;
    for (G391Voice &voice : g_g391Mixer.voices)
    {
        if (!voice.active || voice.released || voice.slot != slot ||
            voice.key != key)
        {
            continue;
        }
        if (voice.id != id && id != 0xFFu)
        {
            continue;
        }
        if (best == nullptr || voice.serial > best->serial)
        {
            best = &voice;
        }
    }
    if (best != nullptr)
    {
        best->released = true;
        best->env.keyOff();
        ++g_g391Mixer.keyOffs;
    }
}

// Live parameter change for a sounding voice: `SE_SetVol` (subtype 0),
// `SE_SetPan` (1) and `SE_SetPitch` (2).
void dc2G391SetVoiceParam(uint32_t slot, uint8_t key, uint8_t id,
                          uint8_t subtype, uint32_t value)
{
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    for (G391Voice &voice : g_g391Mixer.voices)
    {
        if (!voice.active || voice.slot != slot || voice.key != key)
        {
            continue;
        }
        if (voice.id != id && id != 0xFFu)
        {
            continue;
        }
        switch (subtype)
        {
        case 0u:
            // The MIDI-domain channel volume this voice was keyed on with is
            // factored out of `baseGain`, so a live SetVol just re-applies it.
            voice.gain = voice.baseGain *
                         (static_cast<float>(std::min<uint32_t>(value, 127u)) / 127.0f);
            break;
        case 1u:
            g391SetPan(voice, static_cast<float>(std::min<uint32_t>(value, 127u)) / 127.0f);
            break;
        case 2u:
            if (value != 0u && voice.decoded)
            {
                // 14-bit fine pitch, 0x2000 = unity (one octave per 0x1000).
                const double semitones =
                    (static_cast<double>(value) - 8192.0) / 8192.0 * 12.0;
                voice.step = static_cast<double>(voice.decoded->sampleRate) *
                             std::pow(2.0, semitones / 12.0) /
                             static_cast<double>(kG391OutputRate);
            }
            break;
        default:
            break;
        }
    }
}

// SPU2 reverb configuration, straight from the guest's `SetReverb__6CSoundFiii`
// (`sceSdRemote` 0x8130 mode attribute + the 0xb80/0xc80 echo-volume writes).
void dc2G391SetReverb(uint32_t mode, uint32_t echoVolume, bool enabled)
{
    if (g391ReverbDisabled())
    {
        return;
    }
    const float wet =
        enabled ? std::min(1.0f, static_cast<float>(echoVolume) / 32767.0f) : 0.0f;
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    g_g391Mixer.reverb.configure(enabled ? mode : 0u, wet);
    if (g385GameAudioTrace())
    {
        std::fprintf(stderr,
                     "[G391:reverb] mode=%u evol=0x%x wet=%.3f enabled=%u active=%u\n",
                     mode, echoVolume, wet, enabled ? 1u : 0u,
                     g_g391Mixer.reverb.active() ? 1u : 0u);
    }
}

// Opt-in verification (`DC2_G391_SELFTEST=1`). The recorded route only carries
// centre-panned, non-looping one-shots with one envelope family, so "it played"
// proves nothing about the loop, pan, envelope-shape and reverb mechanisms.
// Drive each of them directly and assert the observable outcome.
int dc2G391SelfTest()
{
    int passed = 0;
    int total = 0;
    auto check = [&](const char *name, bool ok)
    {
        ++total;
        passed += ok ? 1 : 0;
        std::fprintf(stderr, "[G391:selftest] %-24s %s\n", name, ok ? "PASS" : "FAIL");
    };

    // 1. Envelope: instant attack, hold, then a finite exponential release.
    {
        G391Adsr adsr{};
        adsr.adsr1 = 0x80FFu; // Am=1, Ar=0 (instant), Dr=0xf, Sl=0xf
        adsr.adsr2 = 0x5FECu; // Sr=0x7f (hold), Rr=0x0c exponential
        G391EnvState env{};
        for (int i = 0; i < 64; ++i)
        {
            env.tick(adsr);
        }
        const bool reachedFull = env.level >= 0x7000;
        int holdLevel = env.level;
        for (int i = 0; i < 44100; ++i)
        {
            env.tick(adsr);
        }
        const bool held = env.level >= holdLevel - 16;
        env.keyOff();
        int releaseTicks = 0;
        while (!env.finished() && releaseTicks < 44100 * 10)
        {
            env.tick(adsr);
            ++releaseTicks;
        }
        check("env attack->full", reachedFull);
        check("env sustain holds", held);
        check("env release ends", env.finished() && releaseTicks > 0);
    }

    // 2. Envelope shape actually differs per program (the whole point of item
    //    "no ADSR"): a slow attack must be quieter than an instant one.
    {
        G391Adsr fast{};
        fast.adsr1 = 0x80FFu;
        G391Adsr slow{};
        slow.adsr1 = 0x9CFFu; // Ar = 0x1c
        G391EnvState envFast{};
        G391EnvState envSlow{};
        for (int i = 0; i < 256; ++i)
        {
            envFast.tick(fast);
            envSlow.tick(slow);
        }
        check("env attack differs", envSlow.level < envFast.level);
    }

    // 3. VAG loop points reach the decoded sample.
    {
        G385ResolvedSample sample;
        sample.sampleRate = 44100u;
        sample.vag.assign(16u * 4u, 0u);
        for (size_t block = 0u; block < 4u; ++block)
        {
            sample.vag[block * 16u + 0u] = 0x00u; // shift 0, filter 0
            sample.vag[block * 16u + 1u] = 0u;
            for (size_t byte = 2u; byte < 16u; ++byte)
            {
                sample.vag[block * 16u + byte] = 0x11u;
            }
        }
        sample.vag[1u * 16u + 1u] = 0x04u;          // loop start
        sample.vag[3u * 16u + 1u] = 0x01u | 0x02u;  // loop end + repeat
        const std::shared_ptr<G385DecodedVag> decoded = g391DecodeVagWithLoops(sample);
        check("vag loop detected", decoded && decoded->loops);
        check("vag loop start", decoded && decoded->loopStart == 28u);
        check("vag play end", decoded && decoded->playEnd == 112u);
    }

    // 4. Pan is equal-power and actually moves the image.
    {
        G391Voice left{};
        G391Voice centre{};
        G391Voice right{};
        g391SetPan(left, 0.0f);
        g391SetPan(centre, 0.5f);
        g391SetPan(right, 1.0f);
        check("pan hard left", left.panLeft > 0.99f && left.panRight < 0.01f);
        check("pan hard right", right.panRight > 0.99f && right.panLeft < 0.01f);
        check("pan centre equal",
              std::fabs(centre.panLeft - centre.panRight) < 0.001f);
    }

    // 5. Reverb produces a decaying tail after the dry impulse stops.
    {
        G391Reverb reverb{};
        reverb.configure(5u, 0.5f); // SD_REV_MODE_HALL
        float tail = 0.0f;
        float outL = 0.0f;
        float outR = 0.0f;
        reverb.process(1.0f, 1.0f, outL, outR);
        for (int i = 0; i < static_cast<int>(kG391OutputRate) / 2; ++i)
        {
            reverb.process(0.0f, 0.0f, outL, outR);
            tail = std::max(tail, std::fabs(outL));
        }
        check("reverb active", reverb.active());
        check("reverb tail", tail > 0.0f);
        check("reverb preset hall",
              std::strcmp(reverb.preset().name, "Hall") == 0);
        G391Reverb off{};
        off.configure(0u, 0.5f);
        check("reverb mode 0 silent", !off.active());
        // G392: every libsd SD_REV_MODE must land on a real preset, and the
        // studio family must map small/medium/large in order.
        check("reverb mode map",
              std::strcmp(kG392ReverbPresets[g392PresetForMode(1u)].name, "Room") == 0 &&
                  std::strcmp(kG392ReverbPresets[g392PresetForMode(2u)].name,
                              "Studio Small") == 0 &&
                  std::strcmp(kG392ReverbPresets[g392PresetForMode(3u)].name,
                              "Studio Medium") == 0 &&
                  std::strcmp(kG392ReverbPresets[g392PresetForMode(4u)].name,
                              "Studio Large") == 0 &&
                  std::strcmp(kG392ReverbPresets[g392PresetForMode(8u)].name,
                              "Delay") == 0 &&
                  std::strcmp(kG392ReverbPresets[g392PresetForMode(9u)].name,
                              "Pipe") == 0);
        check("reverb retail libsd table",
              kG392ReverbPresets[g392PresetForMode(1u)].reg[kDLSAME] == 0x0336u &&
                  kG392ReverbPresets[g392PresetForMode(7u)].reg[kMLSAME] == 0x1FFDu &&
                  kG392ReverbPresets[g392PresetForMode(8u)].reg[kDAPF1] == 0x0003u &&
                  kG392ReverbPresets[g392PresetForMode(9u)].reg[kVWALL] == 0x8500u);
        // The work area must be big enough for the largest tap of each preset.
        bool tapsFit = true;
        for (uint32_t mode = 0u; mode <= 9u; ++mode)
        {
            G391Reverb probe{};
            probe.configure(mode, 0.5f);
            for (int field = 0; field < 32; ++field)
            {
                const uint16_t reg = probe.preset().reg[field];
                if (field >= kMLSAME && field <= kMRAPF2 &&
                    static_cast<size_t>(reg) * 4u > probe.bufferSamples)
                {
                    tapsFit = false;
                }
            }
        }
        check("reverb taps fit", tapsFit);
    }

    std::fprintf(stderr, "[G391:selftest] %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}

// ------------------------------------------------------------- G392 BGM bus --

bool dc2G392BgmBusEnabled()
{
    return !g392BgmBusDisabled();
}

bool dc2G392BgmStart(uint32_t port, std::shared_ptr<std::vector<int16_t>> pcm,
                     uint64_t loopStartFrame, uint64_t loopEndFrame, float volume)
{
    if (g392BgmBusDisabled() || port >= g_g391Mixer.bgm.size() || !pcm ||
        loopEndFrame <= loopStartFrame || !g391EnsureStream())
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
        G392BgmSource &source = g_g391Mixer.bgm[port];
        source.pcm = std::move(pcm);
        source.cursorFrame = 0u;
        source.loopStartFrame = loopStartFrame;
        source.loopEndFrame = loopEndFrame;
        source.volume = volume;
        source.active = true;
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G394:mix] source-start port=%u callbacks=%llu volume=%.3f\n",
                         port, static_cast<unsigned long long>(g_g391Mixer.callbacks),
                         volume);
        }
    }
    return true;
}

void dc2G392BgmStop(uint32_t port)
{
    if (port >= g_g391Mixer.bgm.size())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    G392BgmSource &source = g_g391Mixer.bgm[port];
    constexpr uint64_t kG394EarlyStopFrames = kG391OutputRate / 10u;
    if (!g394AsyncStopGuardDisabled() && source.active &&
        source.earlyMutePending &&
        source.cursorFrame < kG394EarlyStopFrames)
    {
        if (g385GameAudioTrace())
        {
            std::fprintf(
                stderr,
                "[G394:mix] suppress-stale-stop port=%u cursor=%llu\n",
                port, static_cast<unsigned long long>(source.cursorFrame));
        }
        source.earlyMutePending = false;
        return;
    }
    if (g385GameAudioTrace() && source.active)
    {
        std::fprintf(stderr,
                     "[G394:mix] source-stop port=%u callbacks=%llu cursor=%llu active=%d\n",
                     port, static_cast<unsigned long long>(g_g391Mixer.callbacks),
                     static_cast<unsigned long long>(source.cursorFrame),
                     source.active ? 1 : 0);
    }
    source = G392BgmSource{};
}

void dc2G392BgmSetVolume(uint32_t port, float volume)
{
    if (port >= g_g391Mixer.bgm.size())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    G392BgmSource &source = g_g391Mixer.bgm[port];
    constexpr uint64_t kG394EarlyStopFrames = kG391OutputRate / 10u;
    if (!g394AsyncStopGuardDisabled() && source.active &&
        volume <= 0.0f && source.cursorFrame < kG394EarlyStopFrames)
    {
        source.earlyMutePending = true;
        if (g385GameAudioTrace())
        {
            std::fprintf(
                stderr,
                "[G394:mix] defer-early-mute port=%u cursor=%llu\n",
                port, static_cast<unsigned long long>(source.cursorFrame));
        }
        return;
    }
    source.earlyMutePending = false;
    source.volume = volume;
}

void dc2G392BgmRewind(uint32_t port)
{
    if (port >= g_g391Mixer.bgm.size())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    g_g391Mixer.bgm[port].cursorFrame = 0u;
}

void dc2G391StopAllVoices()
{
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    for (G391Voice &voice : g_g391Mixer.voices)
    {
        voice.active = false;
        voice.decoded.reset();
    }
}

void dc2G391ReportMixerStats()
{
    if (!g385GameAudioTrace())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_g391Mixer.mutex);
    size_t sounding = 0u;
    for (const G391Voice &voice : g_g391Mixer.voices)
    {
        sounding += voice.active ? 1u : 0u;
    }
    std::fprintf(stderr,
                 "[G391:mix] keyOns=%llu keyOffs=%llu steals=%llu looped=%llu "
                 "sounding=%zu\n",
                 static_cast<unsigned long long>(g_g391Mixer.keyOns),
                 static_cast<unsigned long long>(g_g391Mixer.keyOffs),
                 static_cast<unsigned long long>(g_g391Mixer.steals),
                 static_cast<unsigned long long>(g_g391Mixer.loopedVoices),
                 sounding);
}
