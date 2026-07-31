namespace
{
    struct G385PendingBank
    {
        std::vector<uint8_t> header;
        std::vector<uint8_t> body;
        // G386.5: a `.BD` body larger than the IOP staging window arrives as
        // several consecutive SIF DMA transfers. Record where each captured
        // chunk starts so the commit can select the chunk-aligned run that
        // matches the size the guest's SetHD actually requested.
        std::vector<size_t> chunkOffsets;
    };

    // Guard against an unbounded pending body if a bank is never committed.
    constexpr size_t kG385MaxPendingBodyBytes = 8u * 1024u * 1024u;

    struct G385MidiBank
    {
        std::vector<uint8_t> header;
        std::vector<uint8_t> body;
        uint64_t generation = 0u;
    };

    struct G385MidiSequence
    {
        std::vector<uint8_t> source;
        std::shared_ptr<std::vector<int16_t>> renderedPcm;
        uint32_t sampleRate = 44100u;
        uint64_t loopStartFrame = 0u;
        uint64_t loopEndFrame = 0u;
        uint64_t generation = 0u;
        uint32_t eventCount = 0u;
        uint32_t noteCount = 0u;
        uint32_t resolveMissCount = 0u;
    };

    struct G385MusicPlayback
    {
        std::mutex mutex;
        AudioStream stream{};
        std::shared_ptr<std::vector<int16_t>> pcm;
        uint64_t cursorFrame = 0u;
        uint64_t loopStartFrame = 0u;
        uint64_t loopEndFrame = 0u;
        float volume = 1.0f;
        bool loaded = false;
        bool active = false;
    };

    struct G385ResolvedSample
    {
        std::vector<uint8_t> vag;
        uint32_t sampleRate = 32000u;
        uint32_t vagIndex = 0u;
        uint8_t baseNote = 60u;
        int8_t sampleDetune = 0;
        int8_t splitTranspose = 0;
        int8_t splitDetune = 0;
        uint8_t programVolume = 127u;
        uint8_t splitVolume = 127u;
        uint8_t sampleVolume = 127u;
        // G391: the Sony `.HD` sample parameter carries the SPU2 envelope
        // (ADSR1 at +18, ADSR2 at +20, little-endian — located by dumping live
        // banks, `DC2_G391_HD_DUMP=1`) and a 0..127 pan at split +17 with the
        // program-wide default at programParam +7 (0x40 = centre).
        uint16_t adsr1 = 0x80FFu;
        uint16_t adsr2 = 0x5FC0u;
        uint8_t programPan = 64u;
        uint8_t splitPan = 64u;
    };

    std::mutex g_g385BankMutex;
    G385PendingBank g_g385PendingBank;
    std::array<G385MidiBank, 16u> g_g385MidiBanks;
    uint64_t g_g385BankGeneration = 0u;
    std::vector<uint8_t> g_g385PendingSequence;
    std::array<G385MidiSequence, 16u> g_g385MidiSequences;
    uint64_t g_g385SequenceGeneration = 0u;
    std::array<G385MusicPlayback, 16u> g_g385MusicPlayback;

    bool g385GameAudioDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G385_NO_GAME_AUDIO");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    bool g385GameAudioTrace()
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("DC2_G385_AUDIO_TRACE");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return enabled;
    }

    // G387 rollback: restore the pre-G387 single-transfer `.BD` capture, which
    // drops every continuation chunk of a split bank body.
    bool g387BdChunksDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G387_NO_BD_CHUNKS");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    // G388 rollback: refuse to re-bind an already-committed bank to a second
    // port, restoring the pre-G388 "one SetHD consumes the capture" behaviour.
    bool g388BankAliasDisabled()
    {
        static const bool disabled = []()
        {
            const char *value = std::getenv("DC2_G388_NO_BANK_ALIAS");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return disabled;
    }

    // G388: DC2 uploads one HD/BD pair and then issues SEVERAL SetHD commands
    // that name the SAME IOP addresses and size, binding that one bank to more
    // than one EZMIDI port (e.g. the SFX port and the battle-BGM port). The
    // pending capture is consumed by the first commit, so remember the last
    // committed bank keyed by its guest identity and re-bind it for the rest.
    struct G385CommittedBankCache
    {
        std::vector<uint8_t> header;
        std::vector<uint8_t> body;
        uint32_t hdAddr = 0u;
        uint32_t bdAddr = 0u;
        uint32_t bodySize = 0u;
        bool valid = false;
    };

    G385CommittedBankCache g_g385LastCommittedBank;

    // G389: the guest's per-port volume reaches EZMIDI on a 0..0x100 scale.
    // `sndSetPortVol__Fif@0x18D420` quantises its 0..1 float to `f * 127`, and
    // `SetVol__6CSoundFii@0x18A2C0` then multiplies by 256/127 (2.015748)
    // unless the value is already the 0x100 unity token. Both music streams and
    // one-shot SFX on a port are attenuated by it.
    constexpr float kG385PortVolumeUnity = 256.0f;
    std::mutex g_g385PortVolumeMutex;
    std::array<float, 16u> g_g385PortVolume = []()
    {
        std::array<float, 16u> volumes{};
        volumes.fill(1.0f);
        return volumes;
    }();

    float g385PortVolume(uint32_t port)
    {
        if (port >= g_g385PortVolume.size())
        {
            return 1.0f;
        }
        std::lock_guard<std::mutex> lock(g_g385PortVolumeMutex);
        return g_g385PortVolume[port];
    }

    // G389 rollback: restore the pre-G389 mix (port volume divided by 127 below
    // the unity token, and SFX ignoring the port volume entirely).
    bool g389LegacyMixEnabled()
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("DC2_G389_LEGACY_MIX");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return enabled;
    }

    bool g385ReadU16(const std::vector<uint8_t> &data, size_t offset, uint16_t &value)
    {
        if (offset > data.size() || data.size() - offset < sizeof(value))
        {
            return false;
        }
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    }

    bool g385ReadU32(const std::vector<uint8_t> &data, size_t offset, uint32_t &value)
    {
        if (offset > data.size() || data.size() - offset < sizeof(value))
        {
            return false;
        }
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    }

    bool g385RangeValid(const std::vector<uint8_t> &data, size_t offset, size_t size)
    {
        return offset <= data.size() && size <= data.size() - offset;
    }

    bool g385IsSonyHd(const uint8_t *data, uint32_t sizeBytes)
    {
        if (data == nullptr || sizeBytes < 80u)
        {
            return false;
        }
        uint32_t creator0 = 0u;
        uint32_t type0 = 0u;
        uint32_t creator1 = 0u;
        uint32_t type1 = 0u;
        std::memcpy(&creator0, data + 0u, 4u);
        std::memcpy(&type0, data + 4u, 4u);
        std::memcpy(&creator1, data + 16u, 4u);
        std::memcpy(&type1, data + 20u, 4u);
        return creator0 == 0x53434549u && type0 == 0x56657273u &&
               creator1 == 0x53434549u && type1 == 0x48656164u;
    }

    uint8_t g385ClampMidiVolume(uint8_t value)
    {
        return std::min<uint8_t>(value, 127u);
    }

    // G391 bring-up probe: dump the raw Sony `.HD` program/split/sample parameter
    // bytes for the first few resolves so the ADSR fields can be located by
    // observation instead of guessed from a struct layout we do not have.
    bool g391HdDumpEnabled()
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("DC2_G391_HD_DUMP");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        return enabled;
    }

    void g391DumpHdBytes(const char *label, const std::vector<uint8_t> &hd,
                         size_t offset, size_t count)
    {
        if (!g385RangeValid(hd, offset, count))
        {
            return;
        }
        std::fprintf(stderr, "[G391:hd] %s off=0x%zx bytes=", label, offset);
        for (size_t i = 0u; i < count; ++i)
        {
            std::fprintf(stderr, "%02x", static_cast<unsigned>(hd[offset + i]));
        }
        std::fprintf(stderr, "\n");
    }

    bool g385ResolveSample(const G385MidiBank &bank,
                           uint32_t program, uint8_t key, uint8_t velocity,
                           G385ResolvedSample &out)
    {
        const std::vector<uint8_t> &hd = bank.header;
        const std::vector<uint8_t> &bd = bank.body;
        if (hd.size() < 80u || bd.empty())
        {
            return false;
        }

        uint32_t bodySize = 0u;
        uint32_t programChunk = 0u;
        uint32_t sampleSetChunk = 0u;
        uint32_t sampleChunk = 0u;
        uint32_t vagInfoChunk = 0u;
        if (!g385ReadU32(hd, 32u, bodySize) ||
            !g385ReadU32(hd, 36u, programChunk) ||
            !g385ReadU32(hd, 40u, sampleSetChunk) ||
            !g385ReadU32(hd, 44u, sampleChunk) ||
            !g385ReadU32(hd, 48u, vagInfoChunk))
        {
            return false;
        }

        uint32_t maxProgram = 0u;
        if (!g385ReadU32(hd, static_cast<size_t>(programChunk) + 12u, maxProgram) ||
            program > maxProgram)
        {
            return false;
        }

        uint32_t programRelative = 0u;
        if (!g385ReadU32(hd, static_cast<size_t>(programChunk) + 16u + program * 4u,
                         programRelative) ||
            programRelative == 0xFFFFFFFFu)
        {
            return false;
        }

        const size_t programParam = static_cast<size_t>(programChunk) + programRelative;
        if (!g385RangeValid(hd, programParam, 36u))
        {
            return false;
        }
        uint32_t splitRelative = 0u;
        if (!g385ReadU32(hd, programParam, splitRelative))
        {
            return false;
        }
        const uint8_t splitCount = hd[programParam + 4u];
        const uint8_t splitSize = hd[programParam + 5u];
        if (splitSize < 20u)
        {
            return false;
        }
        out.programVolume = hd[programParam + 6u];
        out.programPan = hd[programParam + 7u];

        for (uint32_t splitIndex = 0u; splitIndex < splitCount; ++splitIndex)
        {
            const size_t split = programParam + splitRelative +
                                 static_cast<size_t>(splitIndex) * splitSize;
            if (!g385RangeValid(hd, split, 20u))
            {
                return false;
            }

            uint16_t sampleSetIndex = 0u;
            if (!g385ReadU16(hd, split, sampleSetIndex))
            {
                return false;
            }
            const uint8_t keyLow = hd[split + 2u];
            uint8_t keyHigh = hd[split + 4u];
            if (keyHigh < keyLow)
            {
                keyHigh = 127u;
            }
            if (key < keyLow || key > keyHigh)
            {
                continue;
            }

            uint32_t maxSampleSet = 0u;
            if (!g385ReadU32(hd, static_cast<size_t>(sampleSetChunk) + 12u,
                             maxSampleSet) ||
                sampleSetIndex > maxSampleSet)
            {
                continue;
            }
            uint32_t sampleSetRelative = 0u;
            if (!g385ReadU32(hd,
                             static_cast<size_t>(sampleSetChunk) + 16u +
                                 static_cast<size_t>(sampleSetIndex) * 4u,
                             sampleSetRelative) ||
                sampleSetRelative == 0xFFFFFFFFu)
            {
                continue;
            }
            const size_t sampleSet = static_cast<size_t>(sampleSetChunk) + sampleSetRelative;
            if (!g385RangeValid(hd, sampleSet, 4u))
            {
                continue;
            }

            const uint8_t setVelocityLow = hd[sampleSet + 1u];
            const uint8_t setVelocityHigh = hd[sampleSet + 2u];
            const uint8_t sampleCount = hd[sampleSet + 3u];
            if (velocity < setVelocityLow || velocity > setVelocityHigh)
            {
                continue;
            }

            for (uint32_t sampleInSet = 0u; sampleInSet < sampleCount; ++sampleInSet)
            {
                uint16_t sampleIndex = 0u;
                if (!g385ReadU16(hd, sampleSet + 4u + sampleInSet * 2u,
                                 sampleIndex))
                {
                    break;
                }

                uint32_t maxSample = 0u;
                if (!g385ReadU32(hd, static_cast<size_t>(sampleChunk) + 12u,
                                 maxSample) ||
                    sampleIndex > maxSample)
                {
                    continue;
                }
                uint32_t sampleRelative = 0u;
                if (!g385ReadU32(hd,
                                 static_cast<size_t>(sampleChunk) + 16u +
                                     static_cast<size_t>(sampleIndex) * 4u,
                                 sampleRelative))
                {
                    continue;
                }
                const size_t sampleParam = static_cast<size_t>(sampleChunk) + sampleRelative;
                if (!g385RangeValid(hd, sampleParam, 42u))
                {
                    continue;
                }
                const uint8_t sampleVelocityLow = hd[sampleParam + 2u];
                const uint8_t sampleVelocityHigh = hd[sampleParam + 4u];
                if (velocity < sampleVelocityLow || velocity > sampleVelocityHigh)
                {
                    continue;
                }

                uint16_t vagIndex = 0u;
                if (!g385ReadU16(hd, sampleParam, vagIndex))
                {
                    continue;
                }
                uint32_t maxVag = 0u;
                if (!g385ReadU32(hd, static_cast<size_t>(vagInfoChunk) + 12u,
                                 maxVag) ||
                    vagIndex > maxVag)
                {
                    continue;
                }
                uint32_t vagInfoRelative = 0u;
                if (!g385ReadU32(hd,
                                 static_cast<size_t>(vagInfoChunk) + 16u +
                                     static_cast<size_t>(vagIndex) * 4u,
                                 vagInfoRelative))
                {
                    continue;
                }
                const size_t vagInfo = static_cast<size_t>(vagInfoChunk) + vagInfoRelative;
                uint32_t vagOffset = 0u;
                uint16_t sampleRate = 0u;
                if (!g385ReadU32(hd, vagInfo, vagOffset) ||
                    !g385ReadU16(hd, vagInfo + 4u, sampleRate))
                {
                    continue;
                }

                uint32_t vagEnd = static_cast<uint32_t>(bd.size());
                if (vagIndex < maxVag)
                {
                    uint32_t nextInfoRelative = 0u;
                    uint32_t nextOffset = 0u;
                    if (g385ReadU32(hd,
                                    static_cast<size_t>(vagInfoChunk) + 16u +
                                        static_cast<size_t>(vagIndex + 1u) * 4u,
                                    nextInfoRelative) &&
                        g385ReadU32(hd,
                                    static_cast<size_t>(vagInfoChunk) + nextInfoRelative,
                                    nextOffset))
                    {
                        vagEnd = nextOffset;
                    }
                }
                vagEnd = std::min<uint32_t>(vagEnd, static_cast<uint32_t>(bd.size()));
                if (vagOffset >= vagEnd || (vagEnd - vagOffset) < 16u)
                {
                    continue;
                }

                out.vag.assign(bd.begin() + vagOffset, bd.begin() + vagEnd);
                out.sampleRate = sampleRate != 0u ? sampleRate : 32000u;
                out.vagIndex = vagIndex;
                out.baseNote = hd[sampleParam + 11u];
                out.sampleDetune = static_cast<int8_t>(hd[sampleParam + 12u]);
                out.sampleVolume = hd[sampleParam + 16u];
                out.splitVolume = hd[split + 16u];
                out.splitTranspose = static_cast<int8_t>(hd[split + 18u]);
                out.splitDetune = static_cast<int8_t>(hd[split + 19u]);
                out.splitPan = hd[split + 17u];
                uint16_t adsr1 = 0u;
                uint16_t adsr2 = 0u;
                if (g385ReadU16(hd, sampleParam + 18u, adsr1) &&
                    g385ReadU16(hd, sampleParam + 20u, adsr2))
                {
                    out.adsr1 = adsr1;
                    out.adsr2 = adsr2;
                }
                if (g391HdDumpEnabled())
                {
                    static uint32_t dumpCount = 0u;
                    if (dumpCount < 24u)
                    {
                        ++dumpCount;
                        std::fprintf(stderr,
                                     "[G391:hd] resolve program=%u key=%u vel=%u vag=%u "
                                     "splitSize=%u\n",
                                     program, key, velocity, vagIndex, splitSize);
                        g391DumpHdBytes("programParam", hd, programParam, 36u);
                        g391DumpHdBytes("split", hd, split, splitSize);
                        g391DumpHdBytes("sampleParam", hd, sampleParam, 64u);
                        g391DumpHdBytes("vagInfo", hd, vagInfo, 16u);
                    }
                }
                return true;
            }
        }
        return false;
    }

    void g385WriteBe32(std::vector<uint8_t> &data, size_t offset, uint32_t value)
    {
        data[offset + 0u] = static_cast<uint8_t>(value >> 24u);
        data[offset + 1u] = static_cast<uint8_t>(value >> 16u);
        data[offset + 2u] = static_cast<uint8_t>(value >> 8u);
        data[offset + 3u] = static_cast<uint8_t>(value);
    }

    bool g385IsSonySequence(const uint8_t *data, uint32_t sizeBytes)
    {
        if (data == nullptr || sizeBytes < 0x4Au)
        {
            return false;
        }

        uint32_t words[6]{};
        std::memcpy(&words[0], data + 0x00u, 4u);
        std::memcpy(&words[1], data + 0x04u, 4u);
        std::memcpy(&words[2], data + 0x10u, 4u);
        std::memcpy(&words[3], data + 0x14u, 4u);
        std::memcpy(&words[4], data + 0x30u, 4u);
        std::memcpy(&words[5], data + 0x34u, 4u);
        return words[0] == 0x53434549u && words[1] == 0x56657273u &&
               words[2] == 0x53434549u && words[3] == 0x53657175u &&
               words[4] == 0x53434549u && words[5] == 0x4D696469u;
    }

    enum class G385MidiEventType : uint8_t
    {
        NoteOff,
        NoteOn,
        Control,
        Program,
        PitchBend,
    };

    struct G385MidiEvent
    {
        uint64_t frame = 0u;
        G385MidiEventType type = G385MidiEventType::Control;
        uint8_t channel = 0u;
        uint8_t data0 = 0u;
        uint16_t data1 = 0u;
    };

    struct G385ParsedSequence
    {
        std::vector<G385MidiEvent> events;
        uint64_t endFrame = 0u;
        uint64_t loopStartFrame = 0u;
        uint64_t loopEndFrame = 0u;
        uint32_t noteCount = 0u;
    };

    bool g385ReadVarLen(const std::vector<uint8_t> &data, size_t &offset,
                        uint32_t &value)
    {
        value = 0u;
        for (uint32_t byteIndex = 0u; byteIndex < 5u; ++byteIndex)
        {
            if (offset >= data.size())
            {
                return false;
            }
            const uint8_t byte = data[offset++];
            value = (value << 7u) | static_cast<uint32_t>(byte & 0x7Fu);
            if ((byte & 0x80u) == 0u)
            {
                return true;
            }
        }
        return false;
    }

    bool g385ParseSequence(const std::vector<uint8_t> &sequence,
                           uint32_t outputRate, G385ParsedSequence &out)
    {
        out = {};
        if (!g385IsSonySequence(sequence.data(),
                                static_cast<uint32_t>(sequence.size())))
        {
            return false;
        }

        uint32_t versionChunkSize = 0u;
        uint32_t headerChunkSize = 0u;
        if (!g385ReadU32(sequence, 8u, versionChunkSize) ||
            !g385ReadU32(sequence, static_cast<size_t>(versionChunkSize) + 8u,
                         headerChunkSize))
        {
            return false;
        }
        const size_t midiChunk =
            static_cast<size_t>(versionChunkSize) + headerChunkSize;
        uint32_t midiDataRelative = 0u;
        if (!g385ReadU32(sequence, midiChunk + 16u, midiDataRelative))
        {
            return false;
        }
        const size_t midiData = midiChunk + midiDataRelative;
        uint32_t sequenceRelative = 0u;
        uint16_t ppqn = 0u;
        if (!g385ReadU32(sequence, midiData, sequenceRelative) ||
            !g385ReadU16(sequence, midiData + 4u, ppqn) || ppqn == 0u)
        {
            return false;
        }

        size_t offset = midiData + sequenceRelative;
        if (offset >= sequence.size())
        {
            return false;
        }

        uint8_t runningStatus = 0u;
        bool skipDeltaTime = false;
        uint32_t microsPerQuarter = 500000u;
        long double elapsedFrames = 0.0L;
        constexpr uint32_t kMaxEvents = 1000000u;

        auto readByte = [&](uint8_t &value) -> bool
        {
            if (offset >= sequence.size())
            {
                return false;
            }
            value = sequence[offset++];
            return true;
        };
        auto readCompressedDataByte = [&](uint8_t &value) -> bool
        {
            uint8_t raw = 0u;
            if (!readByte(raw))
            {
                return false;
            }
            skipDeltaTime = (raw & 0x80u) != 0u;
            value = raw & 0x7Fu;
            return true;
        };

        while (offset < sequence.size() &&
               out.events.size() < static_cast<size_t>(kMaxEvents))
        {
            uint32_t deltaTicks = 0u;
            if (skipDeltaTime)
            {
                skipDeltaTime = false;
            }
            else if (!g385ReadVarLen(sequence, offset, deltaTicks))
            {
                return false;
            }

            elapsedFrames +=
                static_cast<long double>(deltaTicks) *
                static_cast<long double>(microsPerQuarter) *
                static_cast<long double>(outputRate) /
                (static_cast<long double>(ppqn) * 1000000.0L);
            const uint64_t eventFrame =
                static_cast<uint64_t>(std::llround(elapsedFrames));

            uint8_t status = 0u;
            if (!readByte(status))
            {
                return false;
            }
            if (status <= 0x7Fu)
            {
                if (runningStatus == 0u)
                {
                    return false;
                }
                status = runningStatus;
                --offset;
            }
            else if (status != 0xFFu)
            {
                runningStatus = status;
            }

            const uint8_t channel = status & 0x0Fu;
            const uint8_t family = status & 0xF0u;
            G385MidiEvent event{};
            event.frame = eventFrame;
            event.channel = channel;

            if (family == 0x80u)
            {
                event.type = G385MidiEventType::NoteOff;
                if (!readCompressedDataByte(event.data0))
                {
                    return false;
                }
            }
            else if (family == 0x90u)
            {
                uint8_t velocity = 0u;
                event.type = G385MidiEventType::NoteOn;
                if (!readByte(event.data0) ||
                    !readCompressedDataByte(velocity))
                {
                    return false;
                }
                event.data1 = velocity;
                if (velocity == 0u)
                {
                    event.type = G385MidiEventType::NoteOff;
                }
                else
                {
                    ++out.noteCount;
                }
            }
            else if (family == 0xB0u)
            {
                uint8_t value = 0u;
                event.type = G385MidiEventType::Control;
                if (!readByte(event.data0) ||
                    !readCompressedDataByte(value))
                {
                    return false;
                }
                event.data1 = value;
                if (event.data0 == 99u)
                {
                    if (value == 0u)
                    {
                        out.loopStartFrame = eventFrame;
                    }
                    else if (value == 1u)
                    {
                        out.loopEndFrame = eventFrame;
                    }
                }
            }
            else if (family == 0xC0u)
            {
                event.type = G385MidiEventType::Program;
                if (!readCompressedDataByte(event.data0))
                {
                    return false;
                }
            }
            else if (family == 0xE0u)
            {
                uint8_t lowSeven = 0u;
                uint8_t highSeven = 0u;
                event.type = G385MidiEventType::PitchBend;
                if (!readByte(lowSeven) ||
                    !readCompressedDataByte(highSeven))
                {
                    return false;
                }
                event.data1 =
                    static_cast<uint16_t>((highSeven << 7u) | lowSeven);
            }
            else if (status == 0xFFu)
            {
                uint8_t metaType = 0u;
                if (!readByte(metaType))
                {
                    return false;
                }
                if (metaType == 0x51u)
                {
                    if (!g385RangeValid(sequence, offset, 4u))
                    {
                        return false;
                    }
                    microsPerQuarter =
                        (static_cast<uint32_t>(sequence[offset + 1u]) << 16u) |
                        (static_cast<uint32_t>(sequence[offset + 2u]) << 8u) |
                        static_cast<uint32_t>(sequence[offset + 3u]);
                    if (microsPerQuarter == 0u)
                    {
                        microsPerQuarter = 500000u;
                    }
                    offset += 4u;
                    out.endFrame = eventFrame;
                    continue;
                }
                if (metaType == 0x2Fu)
                {
                    out.endFrame = eventFrame;
                    break;
                }
                return false;
            }
            else
            {
                return false;
            }

            out.events.push_back(event);
            out.endFrame = eventFrame;
        }

        if (out.endFrame == 0u)
        {
            return false;
        }
        if (out.loopEndFrame <= out.loopStartFrame ||
            out.loopEndFrame > out.endFrame)
        {
            out.loopStartFrame = 0u;
            out.loopEndFrame = out.endFrame;
        }
        return true;
    }

    struct G385DecodedVag
    {
        std::vector<int16_t> pcm;
        uint32_t sampleRate = 44100u;
        uint32_t playEnd = 0u;
        uint32_t loopStart = 0u;
        bool loops = false;
    };

    std::vector<uint8_t> g385BuildVagp(const G385ResolvedSample &sample)
    {
        std::vector<uint8_t> vagp(48u + sample.vag.size(), 0u);
        vagp[0] = 'V';
        vagp[1] = 'A';
        vagp[2] = 'G';
        vagp[3] = 'p';
        g385WriteBe32(vagp, 12u, static_cast<uint32_t>(sample.vag.size()));
        g385WriteBe32(vagp, 16u, sample.sampleRate);
        std::memcpy(vagp.data() + 48u, sample.vag.data(), sample.vag.size());
        return vagp;
    }
}

// G391: the live one-shot voice mixer (ADSR, VAG loop points, pan, reverb).
// Reuses G385ResolvedSample / G385DecodedVag / g385BuildVagp above; its ADSR
// state machine and loop-aware VAG decode are shared with the BGM renderer.
#include "dc2_g391_sfx_mixer.inc"

namespace
{
    struct G385RenderInstrument
    {
        std::shared_ptr<G385DecodedVag> decoded;
        uint8_t baseNote = 60u;
        int8_t splitTranspose = 0;
        int16_t detuneCents = 0;
        float baseVolume = 1.0f;
        // G391: per-program SPU2 envelope and HD pan.
        uint16_t adsr1 = 0x80FFu;
        uint16_t adsr2 = 0x5FC0u;
        uint8_t hdPan = 64u;
    };

    struct G385RenderChannel
    {
        uint8_t program = 0u;
        uint8_t volume = 127u;
        uint8_t expression = 127u;
        uint8_t pan = 64u;
        uint16_t pitchBend = 8192u;
        float gain = 1.0f;
        double pitchRatio = 1.0;
    };

    struct G385RenderVoice
    {
        std::shared_ptr<G385RenderInstrument> instrument;
        double sourcePosition = 0.0;
        uint32_t ageFrames = 0u;
        uint32_t releaseFrames = 0u;
        uint8_t channel = 0u;
        uint8_t key = 60u;
        uint8_t velocity = 127u;
        bool releasing = false;
        double sourceStep = 1.0;
        float fixedGain = 1.0f;
        float panLeft = 0.70710678f;
        float panRight = 0.70710678f;
        // G391: real per-program envelope. `releaseFrames` is only still used by
        // the legacy arm (`DC2_G391_NO_ADSR=1`).
        G391EnvState env{};
    };

    std::shared_ptr<G385RenderInstrument> g385GetRenderInstrument(
        const G385MidiBank &bank, uint8_t program, uint8_t key, uint8_t velocity,
        std::unordered_map<uint32_t, std::shared_ptr<G385RenderInstrument>> &zoneCache,
        std::unordered_map<uint32_t, std::shared_ptr<G385DecodedVag>> &vagCache)
    {
        const uint32_t zoneKey =
            (static_cast<uint32_t>(program) << 16u) |
            (static_cast<uint32_t>(key) << 8u) | velocity;
        const auto zoneIt = zoneCache.find(zoneKey);
        if (zoneIt != zoneCache.end())
        {
            return zoneIt->second;
        }

        G385ResolvedSample sample;
        if (!g385ResolveSample(bank, program, key, velocity, sample))
        {
            zoneCache.emplace(zoneKey, nullptr);
            return nullptr;
        }

        std::shared_ptr<G385DecodedVag> decoded;
        const auto vagIt = vagCache.find(sample.vagIndex);
        if (vagIt != vagCache.end())
        {
            decoded = vagIt->second;
        }
        else
        {
            decoded = g391DecodeVagWithLoops(sample);
            if (!decoded)
            {
                zoneCache.emplace(zoneKey, nullptr);
                return nullptr;
            }
            vagCache.emplace(sample.vagIndex, decoded);
        }

        auto instrument = std::make_shared<G385RenderInstrument>();
        instrument->decoded = std::move(decoded);
        instrument->baseNote = sample.baseNote;
        instrument->splitTranspose = sample.splitTranspose;
        instrument->detuneCents =
            static_cast<int16_t>(sample.sampleDetune) +
            static_cast<int16_t>(sample.splitDetune);
        instrument->baseVolume =
            (static_cast<float>(g385ClampMidiVolume(sample.programVolume)) / 127.0f) *
            (static_cast<float>(g385ClampMidiVolume(sample.splitVolume)) / 127.0f) *
            (static_cast<float>(g385ClampMidiVolume(sample.sampleVolume)) / 127.0f);
        instrument->adsr1 = sample.adsr1;
        instrument->adsr2 = sample.adsr2;
        instrument->hdPan = g385ClampMidiVolume(sample.splitPan);
        zoneCache.emplace(zoneKey, instrument);
        return instrument;
    }

    bool g385RenderSequence(const std::vector<uint8_t> &sequence,
                            const G385MidiBank &bank,
                            G385MidiSequence &out)
    {
        constexpr uint32_t kOutputRate = 44100u;
        constexpr uint32_t kReleaseFrames = kOutputRate / 50u;
        constexpr uint32_t kAttackFrames = kOutputRate / 250u;
        constexpr uint64_t kMaximumFrames =
            static_cast<uint64_t>(kOutputRate) * 180u;
        constexpr double kHalfPi = 1.57079632679489661923;

        G385ParsedSequence parsed;
        if (!g385ParseSequence(sequence, kOutputRate, parsed))
        {
            return false;
        }

        const uint64_t renderFrames = std::min<uint64_t>(
            parsed.endFrame + kReleaseFrames, kMaximumFrames);
        if (renderFrames == 0u ||
            renderFrames > static_cast<uint64_t>(SIZE_MAX / (sizeof(float) * 2u)))
        {
            return false;
        }

        std::vector<float> mix(static_cast<size_t>(renderFrames) * 2u, 0.0f);
        std::array<G385RenderChannel, 16u> channels{};
        std::array<bool, 16u> channelPanSeen{};
        std::vector<G385RenderVoice> voices;
        std::unordered_map<uint32_t, std::shared_ptr<G385RenderInstrument>> zoneCache;
        std::unordered_map<uint32_t, std::shared_ptr<G385DecodedVag>> vagCache;
        size_t eventIndex = 0u;
        uint32_t resolveMissCount = 0u;

        // G394: pitch, pan and gain only change at MIDI events. The old inner
        // sample loop recalculated pow/sin/cos and invariant gain products for
        // every active voice at 44.1 kHz, making the first title sequence take
        // nearly the full idle-to-FMV interval to render.
        const auto updateVoicePan =
            [&](G385RenderVoice &voice, uint8_t pan)
        {
            const double angle =
                (static_cast<double>(pan) / 127.0) * kHalfPi;
            voice.panLeft = static_cast<float>(std::cos(angle));
            voice.panRight = static_cast<float>(std::sin(angle));
        };

        for (uint64_t frame = 0u; frame < renderFrames; ++frame)
        {
            while (eventIndex < parsed.events.size() &&
                   parsed.events[eventIndex].frame <= frame)
            {
                const G385MidiEvent &event = parsed.events[eventIndex++];
                G385RenderChannel &channel = channels[event.channel];
                switch (event.type)
                {
                case G385MidiEventType::NoteOn:
                {
                    std::shared_ptr<G385RenderInstrument> instrument =
                        g385GetRenderInstrument(
                            bank, channel.program, event.data0,
                            static_cast<uint8_t>(event.data1), zoneCache, vagCache);
                    if (instrument)
                    {
                        G385RenderVoice voice{};
                        voice.instrument = std::move(instrument);
                        voice.channel = event.channel;
                        voice.key = event.data0;
                        voice.velocity = static_cast<uint8_t>(event.data1);
                        const float semitones =
                            static_cast<float>(
                                static_cast<int>(voice.key) -
                                static_cast<int>(voice.instrument->baseNote) +
                                static_cast<int>(
                                    voice.instrument->splitTranspose)) +
                            static_cast<float>(
                                voice.instrument->detuneCents) /
                                100.0f;
                        voice.sourceStep =
                            static_cast<double>(
                                voice.instrument->decoded->sampleRate) *
                            std::pow(2.0, semitones / 12.0f) /
                            static_cast<double>(kOutputRate);
                        voice.fixedGain =
                            voice.instrument->baseVolume *
                            (static_cast<float>(voice.velocity) / 127.0f);
                        const uint8_t effectivePan =
                            (channelPanSeen[event.channel] ||
                             g391AdsrDisabled())
                                ? channel.pan
                                : voice.instrument->hdPan;
                        updateVoicePan(voice, effectivePan);
                        voices.push_back(std::move(voice));
                    }
                    else
                    {
                        ++resolveMissCount;
                    }
                    break;
                }
                case G385MidiEventType::NoteOff:
                    for (G385RenderVoice &voice : voices)
                    {
                        if (voice.channel == event.channel &&
                            voice.key == event.data0 && !voice.releasing)
                        {
                            voice.releasing = true;
                            voice.releaseFrames = kReleaseFrames;
                            voice.env.keyOff();
                        }
                    }
                    break;
                case G385MidiEventType::Control:
                    if (event.data0 == 7u)
                    {
                        channel.volume = static_cast<uint8_t>(event.data1);
                        channel.gain =
                            (static_cast<float>(channel.volume) / 127.0f) *
                            (static_cast<float>(channel.expression) / 127.0f);
                    }
                    else if (event.data0 == 10u)
                    {
                        channel.pan = static_cast<uint8_t>(event.data1);
                        channelPanSeen[event.channel] = true;
                        for (G385RenderVoice &voice : voices)
                        {
                            if (voice.channel == event.channel)
                            {
                                updateVoicePan(voice, channel.pan);
                            }
                        }
                    }
                    else if (event.data0 == 11u)
                    {
                        channel.expression = static_cast<uint8_t>(event.data1);
                        channel.gain =
                            (static_cast<float>(channel.volume) / 127.0f) *
                            (static_cast<float>(channel.expression) / 127.0f);
                    }
                    break;
                case G385MidiEventType::Program:
                    channel.program = event.data0;
                    break;
                case G385MidiEventType::PitchBend:
                    channel.pitchBend = event.data1;
                    channel.pitchRatio = std::pow(
                        2.0,
                        (((static_cast<double>(channel.pitchBend) - 8192.0) /
                          8192.0) *
                         2.0) /
                            12.0);
                    break;
                }
            }

            for (size_t voiceIndex = 0u; voiceIndex < voices.size();)
            {
                G385RenderVoice &voice = voices[voiceIndex];
                const G385DecodedVag &decoded = *voice.instrument->decoded;
                bool removeVoice =
                    decoded.pcm.empty() || decoded.playEnd == 0u ||
                    (g391AdsrDisabled()
                         ? (voice.releasing && voice.releaseFrames == 0u)
                         : voice.env.finished());

                if (!removeVoice && voice.sourcePosition >= decoded.playEnd)
                {
                    if (decoded.loops)
                    {
                        const double loopLength =
                            static_cast<double>(decoded.playEnd - decoded.loopStart);
                        voice.sourcePosition =
                            static_cast<double>(decoded.loopStart) +
                            std::fmod(voice.sourcePosition - decoded.loopStart,
                                      loopLength);
                    }
                    else
                    {
                        removeVoice = true;
                    }
                }

                if (removeVoice)
                {
                    voices.erase(voices.begin() +
                                 static_cast<std::ptrdiff_t>(voiceIndex));
                    continue;
                }

                const uint32_t sourceIndex =
                    static_cast<uint32_t>(voice.sourcePosition);
                uint32_t nextSourceIndex = sourceIndex + 1u;
                if (nextSourceIndex >= decoded.playEnd)
                {
                    nextSourceIndex =
                        decoded.loops ? decoded.loopStart : sourceIndex;
                }
                if (sourceIndex >= decoded.pcm.size() ||
                    nextSourceIndex >= decoded.pcm.size())
                {
                    voices.erase(voices.begin() +
                                 static_cast<std::ptrdiff_t>(voiceIndex));
                    continue;
                }

                const double fraction =
                    voice.sourcePosition - static_cast<double>(sourceIndex);
                const float sourceSample =
                    static_cast<float>(
                        static_cast<double>(decoded.pcm[sourceIndex]) +
                        (static_cast<double>(decoded.pcm[nextSourceIndex]) -
                         static_cast<double>(decoded.pcm[sourceIndex])) *
                            fraction) /
                    32768.0f;

                const G385RenderChannel &channel = channels[voice.channel];
                // G391: the per-program SPU2 ADSR out of the `.HD` replaces the
                // fixed 4 ms attack / 20 ms release placeholder, so percussive
                // and sustained instruments no longer share one shape.
                float envelopeGain = 1.0f;
                if (g391AdsrDisabled())
                {
                    if (voice.ageFrames < kAttackFrames)
                    {
                        envelopeGain = static_cast<float>(voice.ageFrames) /
                                       static_cast<float>(kAttackFrames);
                    }
                    if (voice.releasing)
                    {
                        envelopeGain *= static_cast<float>(voice.releaseFrames) /
                                        static_cast<float>(kReleaseFrames);
                        --voice.releaseFrames;
                    }
                }
                else
                {
                    G391Adsr adsr{};
                    adsr.adsr1 = voice.instrument->adsr1;
                    adsr.adsr2 = voice.instrument->adsr2;
                    envelopeGain = static_cast<float>(voice.env.tick(adsr)) /
                                   static_cast<float>(kG391EnvMax);
                }
                const float gain =
                    voice.fixedGain * channel.gain * envelopeGain;
                mix[static_cast<size_t>(frame) * 2u + 0u] +=
                    sourceSample * gain * voice.panLeft;
                mix[static_cast<size_t>(frame) * 2u + 1u] +=
                    sourceSample * gain * voice.panRight;

                voice.sourcePosition += voice.sourceStep * channel.pitchRatio;
                ++voice.ageFrames;
                ++voiceIndex;
            }
        }

        // G391: per-sequence peak normalisation (`0.95 / peak`) gave every track
        // the same output level, so a quiet ambient cue came out as loud as a
        // battle theme. Use ONE fixed bus gain for every sequence and take the
        // rare overshoot with a soft knee instead of rescaling the whole track.
        float peak = 0.0f;
        for (float sample : mix)
        {
            peak = std::max(peak, std::abs(sample));
        }
        const bool legacyNormalise = g391LegacyBgmNormaliseEnabled();
        const float scale =
            legacyNormalise ? (peak > 0.95f ? (0.95f / peak) : 1.0f)
                            : kG391SequenceBusGain;
        auto pcm = std::make_shared<std::vector<int16_t>>();
        pcm->resize(mix.size());
        for (size_t sampleIndex = 0u; sampleIndex < mix.size(); ++sampleIndex)
        {
            const float value = mix[sampleIndex] * scale;
            const float scaled =
                legacyNormalise ? std::max(-1.0f, std::min(1.0f, value))
                                : g391SoftLimit(value);
            (*pcm)[sampleIndex] =
                static_cast<int16_t>(std::lround(scaled * 32767.0f));
        }
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G391:bgm] mix peak=%.3f scale=%.3f legacyNorm=%u\n",
                         peak, scale, legacyNormalise ? 1u : 0u);
        }

        out.renderedPcm = std::move(pcm);
        out.sampleRate = kOutputRate;
        out.loopStartFrame =
            std::min<uint64_t>(parsed.loopStartFrame, renderFrames);
        out.loopEndFrame =
            std::min<uint64_t>(parsed.loopEndFrame, renderFrames);
        if (out.loopEndFrame <= out.loopStartFrame)
        {
            out.loopStartFrame = 0u;
            out.loopEndFrame = renderFrames;
        }
        out.eventCount = static_cast<uint32_t>(parsed.events.size());
        out.noteCount = parsed.noteCount;
        out.resolveMissCount = resolveMissCount;
        return !out.renderedPcm->empty();
    }

    template <size_t Port>
    void g385MusicStreamCallback(void *bufferData, unsigned int frames)
    {
        int16_t *output = static_cast<int16_t *>(bufferData);
        if (output == nullptr)
        {
            return;
        }

        G385MusicPlayback &playback = g_g385MusicPlayback[Port];
        std::lock_guard<std::mutex> lock(playback.mutex);
        if (!playback.active || !playback.pcm ||
            playback.loopEndFrame <= playback.loopStartFrame)
        {
            std::memset(output, 0,
                        static_cast<size_t>(frames) * 2u * sizeof(int16_t));
            return;
        }

        unsigned int outputFrame = 0u;
        while (outputFrame < frames)
        {
            if (playback.cursorFrame >= playback.loopEndFrame)
            {
                playback.cursorFrame = playback.loopStartFrame;
            }
            const uint64_t available =
                playback.loopEndFrame - playback.cursorFrame;
            const unsigned int copyFrames = static_cast<unsigned int>(
                std::min<uint64_t>(available, frames - outputFrame));
            const size_t sourceSample =
                static_cast<size_t>(playback.cursorFrame) * 2u;
            const size_t destinationSample =
                static_cast<size_t>(outputFrame) * 2u;
            const size_t copySamples = static_cast<size_t>(copyFrames) * 2u;
            if (sourceSample > playback.pcm->size() ||
                copySamples > playback.pcm->size() - sourceSample)
            {
                std::memset(output + destinationSample, 0,
                            static_cast<size_t>(frames - outputFrame) * 2u *
                                sizeof(int16_t));
                playback.active = false;
                return;
            }
            std::memcpy(output + destinationSample,
                        playback.pcm->data() + sourceSample,
                        copySamples * sizeof(int16_t));
            playback.cursorFrame += copyFrames;
            outputFrame += copyFrames;
        }
    }

    const std::array<AudioCallback, 16u> g_g385MusicCallbacks = {
        g385MusicStreamCallback<0u>,  g385MusicStreamCallback<1u>,
        g385MusicStreamCallback<2u>,  g385MusicStreamCallback<3u>,
        g385MusicStreamCallback<4u>,  g385MusicStreamCallback<5u>,
        g385MusicStreamCallback<6u>,  g385MusicStreamCallback<7u>,
        g385MusicStreamCallback<8u>,  g385MusicStreamCallback<9u>,
        g385MusicStreamCallback<10u>, g385MusicStreamCallback<11u>,
        g385MusicStreamCallback<12u>, g385MusicStreamCallback<13u>,
        g385MusicStreamCallback<14u>, g385MusicStreamCallback<15u>,
    };

    void g385StopMusicPort(uint32_t port)
    {
        if (port >= g_g385MusicPlayback.size())
        {
            return;
        }

#if defined(PLATFORM_VITA)
        (void)port;
#else
        dc2G392BgmStop(port); // G392 bus source (no-op when the bus is off)

        G385MusicPlayback &playback = g_g385MusicPlayback[port];
        AudioStream stream{};
        bool unload = false;
        {
            std::lock_guard<std::mutex> lock(playback.mutex);
            stream = playback.stream;
            unload = playback.loaded;
            playback.active = false;
            playback.loaded = false;
            playback.pcm.reset();
            playback.cursorFrame = 0u;
        }
        if (unload && IsAudioDeviceReady())
        {
            StopAudioStream(stream);
            UnloadAudioStream(stream);
        }
#endif
    }

    void g385StartMusicPort(uint32_t port)
    {
        if (port >= g_g385MusicPlayback.size() || !IsAudioDeviceReady())
        {
            return;
        }

#if defined(PLATFORM_VITA)
        (void)port;
#else
        g385StopMusicPort(port);

        std::shared_ptr<std::vector<int16_t>> pcm;
        uint32_t sampleRate = 44100u;
        uint64_t loopStartFrame = 0u;
        uint64_t loopEndFrame = 0u;
        uint64_t generation = 0u;
        {
            std::lock_guard<std::mutex> lock(g_g385BankMutex);
            const G385MidiSequence &sequence = g_g385MidiSequences[port];
            pcm = sequence.renderedPcm;
            sampleRate = sequence.sampleRate;
            loopStartFrame = sequence.loopStartFrame;
            loopEndFrame = sequence.loopEndFrame;
            generation = sequence.generation;
        }
        if (!pcm || pcm->empty() || loopEndFrame <= loopStartFrame)
        {
            if (g385GameAudioTrace())
            {
                std::fprintf(stderr,
                             "[G385:bgm] start-miss port=%u generation=%llu\n",
                             port, static_cast<unsigned long long>(generation));
            }
            return;
        }

        float portVolume = 1.0f;
        {
            std::lock_guard<std::mutex> lock(g_g385MusicPlayback[port].mutex);
            portVolume = g_g385MusicPlayback[port].volume;
        }

        // G392: route the rendered sequence through the G391 mixer bus so it
        // reaches the same reverb send as the SFX voices (and so there is one
        // master output). The per-port AudioStream stays as the rollback arm.
        if (dc2G392BgmStart(port, pcm, loopStartFrame, loopEndFrame, portVolume))
        {
            std::lock_guard<std::mutex> lock(g_g385MusicPlayback[port].mutex);
            g_g385MusicPlayback[port].active = true;
            g_g385MusicPlayback[port].loopStartFrame = loopStartFrame;
            g_g385MusicPlayback[port].loopEndFrame = loopEndFrame;
            g_g385MusicPlayback[port].cursorFrame = 0u;
            if (g385GameAudioTrace())
            {
                std::fprintf(stderr,
                             "[G385:bgm] start port=%u frames=%llu loop=%llu..%llu "
                             "rate=%u volume=%.3f generation=%llu bus=1\n",
                             port,
                             static_cast<unsigned long long>(loopEndFrame),
                             static_cast<unsigned long long>(loopStartFrame),
                             static_cast<unsigned long long>(loopEndFrame),
                             sampleRate, portVolume,
                             static_cast<unsigned long long>(generation));
            }
            return;
        }

        AudioStream stream = LoadAudioStream(sampleRate, 16u, 2u);
        if (!IsAudioStreamValid(stream))
        {
            return;
        }
        SetAudioStreamCallback(stream, g_g385MusicCallbacks[port]);

        G385MusicPlayback &playback = g_g385MusicPlayback[port];
        float volume = 1.0f;
        {
            std::lock_guard<std::mutex> lock(playback.mutex);
            playback.stream = stream;
            playback.pcm = std::move(pcm);
            playback.cursorFrame = 0u;
            playback.loopStartFrame = loopStartFrame;
            playback.loopEndFrame = loopEndFrame;
            playback.loaded = true;
            playback.active = true;
            volume = playback.volume;
        }
        SetAudioStreamVolume(stream, volume);
        PlayAudioStream(stream);
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G385:bgm] start port=%u frames=%llu loop=%llu..%llu "
                         "rate=%u volume=%.3f generation=%llu\n",
                         port,
                         static_cast<unsigned long long>(loopEndFrame),
                         static_cast<unsigned long long>(loopStartFrame),
                         static_cast<unsigned long long>(loopEndFrame),
                         sampleRate, volume,
                         static_cast<unsigned long long>(generation));
        }
#endif
    }

    void g385SetMusicPortVolume(uint32_t port, uint32_t rawVolume)
    {
        if (port >= g_g385MusicPlayback.size())
        {
            return;
        }

        // G389: one scale, always 0..0x100 (see kG385PortVolumeUnity). The old
        // "<=127 means a 0..127 scale" heuristic doubled every sub-unity port
        // volume and made the mapping discontinuous at 127→128.
        const float divisor = g389LegacyMixEnabled()
                                  ? (rawVolume > 127u ? 256.0f : 127.0f)
                                  : kG385PortVolumeUnity;
        const float volume = std::max(
            0.0f, std::min(1.0f, static_cast<float>(rawVolume) / divisor));
        {
            std::lock_guard<std::mutex> lock(g_g385PortVolumeMutex);
            g_g385PortVolume[port] = volume;
        }
        G385MusicPlayback &playback = g_g385MusicPlayback[port];
        AudioStream stream{};
        bool loaded = false;
        {
            std::lock_guard<std::mutex> lock(playback.mutex);
            playback.volume = volume;
            stream = playback.stream;
            loaded = playback.loaded;
        }
        dc2G392BgmSetVolume(port, volume);
#if !defined(PLATFORM_VITA)
        if (loaded && IsAudioDeviceReady())
        {
            SetAudioStreamVolume(stream, volume);
        }
#endif
    }
}

void dc2G385CaptureIopDma(const uint8_t *data, uint32_t sizeBytes)
{
    if (g385GameAudioDisabled() || data == nullptr || sizeBytes == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_g385BankMutex);
    if (g385IsSonySequence(data, sizeBytes))
    {
        g_g385PendingSequence.assign(data, data + sizeBytes);
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr, "[G385:bgm] captured-sq size=0x%x\n", sizeBytes);
        }
        return;
    }

    if (g385IsSonyHd(data, sizeBytes))
    {
        g_g385PendingBank = {};
        g_g385PendingBank.header.assign(data, data + sizeBytes);
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr, "[G385:bank] captured-hd size=0x%x\n", sizeBytes);
        }
        return;
    }

    if (g387BdChunksDisabled())
    {
        if (!g_g385PendingBank.header.empty() && g_g385PendingBank.body.empty())
        {
            g_g385PendingBank.chunkOffsets.push_back(0u);
            g_g385PendingBank.body.assign(data, data + sizeBytes);
            if (g385GameAudioTrace())
            {
                std::fprintf(stderr, "[G385:bank] captured-bd size=0x%x\n", sizeBytes);
            }
        }
        return;
    }

    if (!g_g385PendingBank.header.empty() &&
        g_g385PendingBank.body.size() + sizeBytes <= kG385MaxPendingBodyBytes)
    {
        g_g385PendingBank.chunkOffsets.push_back(g_g385PendingBank.body.size());
        g_g385PendingBank.body.insert(g_g385PendingBank.body.end(), data,
                                      data + sizeBytes);
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G385:bank] captured-bd size=0x%x chunk=%zu total=0x%zx\n",
                         sizeBytes, g_g385PendingBank.chunkOffsets.size(),
                         g_g385PendingBank.body.size());
        }
    }
}

void dc2G385CommitMidiBank(uint32_t port, uint32_t bodySize, uint32_t hdAddr,
                           uint32_t bdAddr)
{
    if (g385GameAudioDisabled() || port >= g_g385MidiBanks.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_g385BankMutex);
    // The pending body is the concatenation of every `.BD` transfer seen since
    // the header. The guest tells us the true body size here, so accept the
    // chunk-aligned run that ends at the newest chunk and totals exactly that
    // many bytes; anything before it belonged to an earlier, abandoned upload.
    size_t bodyStart = g_g385PendingBank.body.size();
    bool matched = false;
    if (!g_g385PendingBank.header.empty())
    {
        for (size_t i = g_g385PendingBank.chunkOffsets.size(); i-- > 0;)
        {
            const size_t offset = g_g385PendingBank.chunkOffsets[i];
            const size_t span = g_g385PendingBank.body.size() - offset;
            if (span == bodySize)
            {
                bodyStart = offset;
                matched = true;
                break;
            }
            if (span > bodySize)
            {
                break;
            }
        }
    }

    if (!matched)
    {
        // G388: the guest is re-binding the bank it just uploaded to another
        // port. Identity is the SetHD triple (HD address, BD address, size) —
        // no capture is expected on the wire for these repeats.
        const bool aliases = !g388BankAliasDisabled() &&
                             g_g385LastCommittedBank.valid &&
                             g_g385LastCommittedBank.hdAddr == hdAddr &&
                             g_g385LastCommittedBank.bdAddr == bdAddr &&
                             g_g385LastCommittedBank.bodySize == bodySize;
        if (aliases)
        {
            G385MidiBank &alias = g_g385MidiBanks[port];
            alias.header = g_g385LastCommittedBank.header;
            alias.body = g_g385LastCommittedBank.body;
            alias.generation = ++g_g385BankGeneration;
            if (g385GameAudioTrace())
            {
                std::fprintf(stderr,
                             "[G385:bank] commit-alias port=%u hd=0x%zx bd=0x%zx "
                             "hdAddr=0x%x bdAddr=0x%x generation=%llu\n",
                             port, alias.header.size(), alias.body.size(),
                             hdAddr, bdAddr,
                             static_cast<unsigned long long>(alias.generation));
            }
            return;
        }

        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G385:bank] commit-miss port=%u requested=0x%x pending=0x%zx chunks=%zu\n",
                         port, bodySize, g_g385PendingBank.body.size(),
                         g_g385PendingBank.chunkOffsets.size());
        }
        return;
    }

    G385MidiBank &bank = g_g385MidiBanks[port];
    bank.header = std::move(g_g385PendingBank.header);
    bank.body.assign(g_g385PendingBank.body.begin() +
                         static_cast<std::ptrdiff_t>(bodyStart),
                     g_g385PendingBank.body.end());
    bank.generation = ++g_g385BankGeneration;
    g_g385PendingBank = {};

    g_g385LastCommittedBank.header = bank.header;
    g_g385LastCommittedBank.body = bank.body;
    g_g385LastCommittedBank.hdAddr = hdAddr;
    g_g385LastCommittedBank.bdAddr = bdAddr;
    g_g385LastCommittedBank.bodySize = bodySize;
    g_g385LastCommittedBank.valid = true;
    if (g385GameAudioTrace())
    {
        std::fprintf(stderr,
                     "[G385:bank] committed port=%u hd=0x%zx bd=0x%zx generation=%llu\n",
                     port, bank.header.size(), bank.body.size(),
                     static_cast<unsigned long long>(bank.generation));
    }
}

void dc2G385CommitMidiSequence(uint32_t port)
{
    if (g385GameAudioDisabled() || port >= g_g385MidiSequences.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_g385BankMutex);
    if (g_g385PendingSequence.empty())
    {
        if (g385GameAudioTrace())
        {
            std::fprintf(stderr,
                         "[G385:bgm] commit-reuse port=%u generation=%llu\n",
                         port,
                         static_cast<unsigned long long>(
                             g_g385MidiSequences[port].generation));
        }
        return;
    }

    G385MidiSequence rendered{};
    rendered.source = std::move(g_g385PendingSequence);
    rendered.generation = ++g_g385SequenceGeneration;
    const auto renderStarted = std::chrono::steady_clock::now();
    const bool renderedOk =
        g385RenderSequence(rendered.source, g_g385MidiBanks[port], rendered);
    const auto renderElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - renderStarted)
            .count();
    g_g385MidiSequences[port] = std::move(rendered);

    if (g385GameAudioTrace())
    {
        const G385MidiSequence &sequence = g_g385MidiSequences[port];
        uint64_t firstAudibleFrame = 0u;
        if (sequence.renderedPcm)
        {
            const auto firstAudible = std::find_if(
                sequence.renderedPcm->begin(), sequence.renderedPcm->end(),
                [](int16_t sample) { return sample != 0; });
            firstAudibleFrame =
                static_cast<uint64_t>(
                    std::distance(sequence.renderedPcm->begin(), firstAudible)) /
                2u;
        }
        std::fprintf(
            stderr,
            "[G385:bgm] committed port=%u sq=0x%zx rendered=%u "
            "events=%u notes=%u misses=%u frames=%llu loop=%llu..%llu "
            "firstAudible=%llu renderMs=%lld generation=%llu\n",
            port, sequence.source.size(), renderedOk ? 1u : 0u,
            sequence.eventCount, sequence.noteCount,
            sequence.resolveMissCount,
            static_cast<unsigned long long>(
                sequence.renderedPcm ? sequence.renderedPcm->size() / 2u : 0u),
            static_cast<unsigned long long>(sequence.loopStartFrame),
            static_cast<unsigned long long>(sequence.loopEndFrame),
            static_cast<unsigned long long>(firstAudibleFrame),
            static_cast<long long>(renderElapsedMs),
            static_cast<unsigned long long>(sequence.generation));
    }
}

void dc2G385HandleMidiCommand(uint32_t family, uint32_t port,
                              uint32_t arg0)
{
    if (g385GameAudioDisabled() || port >= g_g385MidiSequences.size())
    {
        return;
    }

    switch (family)
    {
    case 0x0000u:
        g385StartMusicPort(port);
        break;
    case 0x0020u:
        // G450: an EZMIDI Stop owns the whole port, not just its BGM source.
        // `sndSeAllStop` -> `SeAllStop_Sub` -> `Stop__6CSound` is exactly this
        // command, so SFX voices on the port must go silent with it.
        g385StopMusicPort(port);
        dc2G391StopSlotVoices(port);
        break;
    case 0x0040u:
        // SetSq replaces the port's sequence; it does not stop its SFX voices.
        g385StopMusicPort(port);
        break;
    case 0x0030u:
    {
        dc2G392BgmRewind(port);
        G385MusicPlayback &playback = g_g385MusicPlayback[port];
        std::lock_guard<std::mutex> lock(playback.mutex);
        playback.cursorFrame = 0u;
        break;
    }
    case 0x00B0u:
        g385SetMusicPortVolume(port, arg0);
        break;
    case 0x80F0u:
        for (uint32_t musicPort = 0u;
             musicPort < g_g385MusicPlayback.size(); ++musicPort)
        {
            g385StopMusicPort(musicPort);
        }
        // G450: EZMIDI Quit tears the module down; nothing may survive it.
        dc2G391StopAllVoices();
        break;
    default:
        break;
    }
}

// `channelPan` is the guest's `F9 01` pan (0..127); 0xFF means the port has not
// sent one yet, in which case the HD split pan is used.
void dc2G385PlaySfx(PS2AudioBackend *audioBackend, uint32_t soundSlot,
                    uint8_t program, uint8_t key, uint8_t velocity,
                    uint8_t channelVolume, uint8_t voiceId, uint8_t channelPan)
{
    if (g385GameAudioDisabled() || audioBackend == nullptr ||
        soundSlot >= g_g385MidiBanks.size() || !IsAudioDeviceReady())
    {
        return;
    }

    G385ResolvedSample sample;
    uint64_t bankGeneration = 0u;
    {
        std::lock_guard<std::mutex> lock(g_g385BankMutex);
        const G385MidiBank &bank = g_g385MidiBanks[soundSlot];
        bankGeneration = bank.generation;
        if (!g385ResolveSample(bank, program, key, velocity, sample))
        {
            if (g385GameAudioTrace())
            {
                // G391: report what the bank on this slot actually holds. A
                // residual miss is either "no bank here" (hd=0) or "this bank
                // has no such program" (program > maxProgram) — the two need
                // completely different follow-ups.
                uint32_t programChunk = 0u;
                uint32_t maxProgram = 0u;
                if (bank.header.size() >= 80u)
                {
                    g385ReadU32(bank.header, 36u, programChunk);
                    g385ReadU32(bank.header,
                                static_cast<size_t>(programChunk) + 12u, maxProgram);
                }
                std::fprintf(stderr,
                             "[G385:sfx] resolve-miss slot=%u program=%u key=%u vel=%u "
                             "hd=0x%zx bd=0x%zx maxProgram=%u generation=%llu\n",
                             soundSlot, program, key, velocity,
                             bank.header.size(), bank.body.size(), maxProgram,
                             static_cast<unsigned long long>(bankGeneration));
            }
            return;
        }
    }

    const uint32_t sampleKey =
        0x01000000u | ((soundSlot & 0x0Fu) << 16u) | (sample.vagIndex & 0xFFFFu);

    const float semitones =
        static_cast<float>(static_cast<int>(key) - static_cast<int>(sample.baseNote) +
                           static_cast<int>(sample.splitTranspose)) +
        static_cast<float>(static_cast<int>(sample.sampleDetune) +
                           static_cast<int>(sample.splitDetune)) /
            100.0f;
    const float pitch = std::pow(2.0f, semitones / 12.0f);
    // G389: a one-shot is attenuated by its port's volume exactly like the
    // sequenced stream on that port (the menu/pause path lowers SE ports).
    const float portVolume =
        g389LegacyMixEnabled() ? 1.0f : g385PortVolume(soundSlot);
    const float volume =
        portVolume *
        (static_cast<float>(g385ClampMidiVolume(sample.programVolume)) / 127.0f) *
        (static_cast<float>(g385ClampMidiVolume(sample.splitVolume)) / 127.0f) *
        (static_cast<float>(g385ClampMidiVolume(sample.sampleVolume)) / 127.0f) *
        (static_cast<float>(velocity) / 127.0f) *
        (static_cast<float>(g385ClampMidiVolume(channelVolume)) / 127.0f);

    // G391: the split's own pan (HD split +17, 0x40 = centre) is the one-shot's
    // position. `PS2AudioBackend::play` has no pan argument at all, which is
    // why every one-shot used to be centred.
    // G392: the guest's own per-SE pan (`F9 01`) wins over the HD default.
    const float pan =
        static_cast<float>(g385ClampMidiVolume(channelPan != 0xFFu ? channelPan
                                                                  : sample.splitPan)) /
        127.0f;
    const float channelGain =
        static_cast<float>(g385ClampMidiVolume(channelVolume)) / 127.0f;

    if (g385GameAudioTrace())
    {
        std::fprintf(stderr,
                     "[G385:sfx] play slot=%u program=%u key=%u vel=%u vag=%u "
                     "rate=%u bytes=0x%zx pitch=%.3f volume=%.3f portVol=%.3f "
                     "pan=%.3f adsr=%04x/%04x id=%u generation=%llu\n",
                     soundSlot, program, key, velocity, sample.vagIndex,
                     sample.sampleRate, sample.vag.size(), pitch, volume,
                     portVolume, pan, sample.adsr1, sample.adsr2, voiceId,
                     static_cast<unsigned long long>(bankGeneration));
    }

    if (!g391LegacySfxPathEnabled())
    {
        dc2G391KeyOn(soundSlot, key, voiceId, sample, pitch, volume, pan,
                     channelGain > 0.0f ? volume / channelGain : volume);
        return;
    }

    const std::vector<uint8_t> vagp = g385BuildVagp(sample);
    audioBackend->onVagTransferFromBuffer(
        vagp.data(), static_cast<uint32_t>(vagp.size()), sampleKey);
    audioBackend->play(sampleKey, pitch, volume);
}

// G391: MODMSIN key-off / live parameter changes for a sounding one-shot.
void dc2G385StopSfx(uint32_t soundSlot, uint8_t key, uint8_t voiceId)
{
    if (g385GameAudioDisabled() || g391LegacySfxPathEnabled())
    {
        return;
    }
    dc2G391KeyOff(soundSlot, key, voiceId);
}

void dc2G385SetSfxParam(uint32_t soundSlot, uint8_t key, uint8_t voiceId,
                        uint8_t subtype, uint32_t value)
{
    if (g385GameAudioDisabled() || g391LegacySfxPathEnabled())
    {
        return;
    }
    dc2G391SetVoiceParam(soundSlot, key, voiceId, subtype, value);
}
