namespace
{
    constexpr uint32_t kG386VoicePortCount = 16u;
    constexpr uint32_t kG386SectorSize = 2048u;
    constexpr uint32_t kG386StatePlaying = 0x1000u;

    struct G386SoundArchiveEntry
    {
        uint32_t size = 0u;
        uint32_t sectorOffset = 0u;
        uint32_t sectorCount = 0u;
    };

    struct G386VoicePort
    {
        Sound sound{};
        bool soundLoaded = false;
        bool opened = false;
        bool playing = false;
        std::string filename;
        float volume = 1.0f;
        uint32_t sampleRate = 0u;
        uint32_t frameCount = 0u;
        std::chrono::steady_clock::time_point deadline{};
    };

    std::mutex g_g386VoiceMutex;
    std::once_flag g_g386SoundIndexOnce;
    std::unordered_map<std::string, G386SoundArchiveEntry> g_g386SoundIndex;
    IsoFileInfo g_g386SoundDatInfo{};
    bool g_g386SoundIndexReady = false;
    std::array<G386VoicePort, kG386VoicePortCount> g_g386VoicePorts;

    bool g386EnvFlagEnabled(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr &&
               value[0] != '\0' &&
               std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0 &&
               std::strcmp(value, "FALSE") != 0 &&
               std::strcmp(value, "off") != 0 &&
               std::strcmp(value, "OFF") != 0;
    }

    bool g386VoiceDisabled()
    {
        static const bool disabled = g386EnvFlagEnabled("DC2_G386_NO_VOICE");
        return disabled;
    }

    bool g386VoiceTraceEnabled()
    {
        static const bool enabled =
            g386EnvFlagEnabled("DC2_G386_VOICE_TRACE") ||
            g386EnvFlagEnabled("DC2_G385_AUDIO_TRACE");
        return enabled;
    }

    uint32_t g386ReadU32(const uint8_t *data, size_t size, size_t offset)
    {
        if (data == nullptr || offset + sizeof(uint32_t) > size)
        {
            return 0u;
        }

        return static_cast<uint32_t>(data[offset + 0u]) |
               (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
               (static_cast<uint32_t>(data[offset + 2u]) << 16u) |
               (static_cast<uint32_t>(data[offset + 3u]) << 24u);
    }

    std::string g386NormalizeFilename(const char *filename)
    {
        if (filename == nullptr)
        {
            return {};
        }

        std::string normalized(filename);
        const size_t slash = normalized.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            normalized.erase(0u, slash + 1u);
        }
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return normalized;
    }

    bool g386ReadIsoFile(const IsoFileInfo &info, std::vector<uint8_t> &out)
    {
        if (info.is_dir || info.size == 0u)
        {
            return false;
        }

        const uint32_t sectorCount =
            (info.size + kG386SectorSize - 1u) / kG386SectorSize;
        out.assign(static_cast<size_t>(sectorCount) * kG386SectorSize, 0u);
        for (uint32_t sector = 0u; sector < sectorCount; ++sector)
        {
            if (!getGlobalIsoMount().readSector(
                    info.lba + sector, 1u,
                    out.data() + static_cast<size_t>(sector) * kG386SectorSize))
            {
                out.clear();
                return false;
            }
        }
        out.resize(info.size);
        return true;
    }

    bool g386EnsureIsoOpen()
    {
        Ps2IsoMount &iso = getGlobalIsoMount();
        if (iso.isOpen())
        {
            return true;
        }

        const char *candidates[] = {
            "D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso",
            "Dark Cloud 2 (USA) (v2.00).iso",
        };
        for (const char *candidate : candidates)
        {
            if (iso.open(candidate))
            {
                return true;
            }
        }
        return false;
    }

    void g386LoadSoundIndex()
    {
        if (!g386EnsureIsoOpen())
        {
            std::fprintf(stderr, "[G386:voice] ISO unavailable\n");
            return;
        }

        IsoFileInfo headerInfo{};
        if (!getGlobalIsoMount().findFile("/SOUND.HD3", headerInfo) ||
            !getGlobalIsoMount().findFile("/SOUND.DAT", g_g386SoundDatInfo))
        {
            std::fprintf(stderr, "[G386:voice] SOUND.HD3/SOUND.DAT unavailable\n");
            return;
        }

        std::vector<uint8_t> header;
        if (!g386ReadIsoFile(headerInfo, header) || header.size() < 16u)
        {
            std::fprintf(stderr, "[G386:voice] failed to read SOUND.HD3\n");
            return;
        }

        const uint32_t firstNameOffset =
            g386ReadU32(header.data(), header.size(), 0u);
        if (firstNameOffset < 16u ||
            firstNameOffset > header.size() ||
            (firstNameOffset % 16u) != 0u)
        {
            std::fprintf(stderr,
                         "[G386:voice] invalid SOUND.HD3 first-name offset 0x%x\n",
                         firstNameOffset);
            return;
        }

        const uint32_t entryCount = firstNameOffset / 16u;
        for (uint32_t index = 0u; index < entryCount; ++index)
        {
            const size_t entryOffset = static_cast<size_t>(index) * 16u;
            const uint32_t nameOffset =
                g386ReadU32(header.data(), header.size(), entryOffset + 0u);
            const uint32_t size =
                g386ReadU32(header.data(), header.size(), entryOffset + 4u);
            const uint32_t sectorOffset =
                g386ReadU32(header.data(), header.size(), entryOffset + 8u);
            const uint32_t sectorCount =
                g386ReadU32(header.data(), header.size(), entryOffset + 12u);
            if (nameOffset < firstNameOffset || nameOffset >= header.size() ||
                size == 0u || sectorCount == 0u ||
                static_cast<uint64_t>(size) >
                    static_cast<uint64_t>(sectorCount) * kG386SectorSize)
            {
                continue;
            }

            size_t nameEnd = nameOffset;
            while (nameEnd < header.size() && header[nameEnd] != 0u)
            {
                ++nameEnd;
            }
            if (nameEnd == header.size())
            {
                continue;
            }

            const std::string rawName(
                reinterpret_cast<const char *>(header.data() + nameOffset),
                nameEnd - nameOffset);
            const std::string name = g386NormalizeFilename(rawName.c_str());
            if (!name.empty())
            {
                g_g386SoundIndex.emplace(
                    name,
                    G386SoundArchiveEntry{size, sectorOffset, sectorCount});
            }
        }

        g_g386SoundIndexReady = !g_g386SoundIndex.empty();
        if (g386VoiceTraceEnabled())
        {
            std::fprintf(stderr,
                         "[G386:voice] SOUND index entries=%zu datLba=0x%x ready=%u\n",
                         g_g386SoundIndex.size(), g_g386SoundDatInfo.lba,
                         g_g386SoundIndexReady ? 1u : 0u);
        }
    }

    bool g386ReadSoundClip(const std::string &filename,
                           std::vector<uint8_t> &out)
    {
        std::call_once(g_g386SoundIndexOnce, g386LoadSoundIndex);
        if (!g_g386SoundIndexReady)
        {
            return false;
        }

        const auto entry = g_g386SoundIndex.find(filename);
        if (entry == g_g386SoundIndex.end())
        {
            return false;
        }

        const G386SoundArchiveEntry &clip = entry->second;
        out.assign(static_cast<size_t>(clip.sectorCount) * kG386SectorSize, 0u);
        for (uint32_t sector = 0u; sector < clip.sectorCount; ++sector)
        {
            if (!getGlobalIsoMount().readSector(
                    g_g386SoundDatInfo.lba + clip.sectorOffset + sector, 1u,
                    out.data() + static_cast<size_t>(sector) * kG386SectorSize))
            {
                out.clear();
                return false;
            }
        }
        out.resize(clip.size);
        return true;
    }

    void g386StopPortLocked(G386VoicePort &port, bool unload)
    {
#if !defined(PLATFORM_VITA)
        if (port.soundLoaded)
        {
            StopSound(port.sound);
            if (unload)
            {
                UnloadSound(port.sound);
                port.sound = {};
                port.soundLoaded = false;
            }
        }
#else
        (void)unload;
#endif
        port.playing = false;
        port.deadline = {};
        if (unload)
        {
            port.opened = false;
            port.filename.clear();
            port.sampleRate = 0u;
            port.frameCount = 0u;
        }
    }

    void g386RefreshPortLocked(uint32_t portIndex)
    {
        G386VoicePort &port = g_g386VoicePorts[portIndex];
        if (!port.playing ||
            std::chrono::steady_clock::now() < port.deadline)
        {
            return;
        }

        g386StopPortLocked(port, false);
        if (g386VoiceTraceEnabled())
        {
            std::fprintf(stderr,
                         "[G386:voice] done port=%u file=\"%s\"\n",
                         portIndex, port.filename.c_str());
        }
    }

    bool g386OpenPortLocked(uint32_t portIndex, const char *filename)
    {
        G386VoicePort &port = g_g386VoicePorts[portIndex];
        g386StopPortLocked(port, true);
        port.volume = 1.0f;

        const std::string normalized = g386NormalizeFilename(filename);
        std::vector<uint8_t> clip;
        if (normalized.empty() || !g386ReadSoundClip(normalized, clip))
        {
            if (g386VoiceTraceEnabled())
            {
                std::fprintf(stderr,
                             "[G386:voice] open-miss port=%u file=\"%s\"\n",
                             portIndex, normalized.c_str());
            }
            return false;
        }

#if defined(PLATFORM_VITA)
        (void)clip;
        return false;
#else
        if (!IsAudioDeviceReady())
        {
            std::fprintf(stderr, "[G386:voice] audio device unavailable\n");
            return false;
        }

        Wave wave = LoadWaveFromMemory(
            ".wav", clip.data(), static_cast<int>(clip.size()));
        if (wave.frameCount == 0u || wave.sampleRate == 0u)
        {
            if (wave.data != nullptr)
            {
                UnloadWave(wave);
            }
            std::fprintf(stderr,
                         "[G386:voice] WAV decode failed file=\"%s\"\n",
                         normalized.c_str());
            return false;
        }

        port.sound = LoadSoundFromWave(wave);
        port.soundLoaded = true;
        port.opened = true;
        port.filename = normalized;
        port.sampleRate = wave.sampleRate;
        port.frameCount = wave.frameCount;
        UnloadWave(wave);
        SetSoundVolume(port.sound, port.volume);

        if (g386VoiceTraceEnabled())
        {
            std::fprintf(
                stderr,
                "[G386:voice] open-ok port=%u file=\"%s\" bytes=%zu rate=%u frames=%u duration=%.3f\n",
                portIndex, normalized.c_str(), clip.size(), port.sampleRate,
                port.frameCount,
                static_cast<double>(port.frameCount) /
                    static_cast<double>(port.sampleRate));
        }
        return true;
#endif
    }

    void g386SetVolumeLocked(uint32_t portIndex, uint32_t packedVolume)
    {
        G386VoicePort &port = g_g386VoicePorts[portIndex];
        const int32_t left = static_cast<int16_t>(packedVolume >> 16u);
        const int32_t right = static_cast<int16_t>(packedVolume & 0xFFFFu);
        const float magnitude =
            static_cast<float>((std::abs(left) + std::abs(right)) / 2) /
            32767.0f;
        port.volume = std::clamp(magnitude, 0.0f, 1.0f);
#if !defined(PLATFORM_VITA)
        if (port.soundLoaded)
        {
            SetSoundVolume(port.sound, port.volume);
        }
#endif
        if (g386VoiceTraceEnabled())
        {
            std::fprintf(stderr,
                         "[G386:voice] volume port=%u raw=0x%08x gain=%.3f\n",
                         portIndex, packedVolume, port.volume);
        }
    }

    void g386PlayPortLocked(uint32_t portIndex)
    {
        G386VoicePort &port = g_g386VoicePorts[portIndex];
        if (!port.opened || !port.soundLoaded ||
            port.sampleRate == 0u || port.frameCount == 0u)
        {
            if (g386VoiceTraceEnabled())
            {
                std::fprintf(stderr,
                             "[G386:voice] play-miss port=%u opened=%u loaded=%u\n",
                             portIndex, port.opened ? 1u : 0u,
                             port.soundLoaded ? 1u : 0u);
            }
            return;
        }

#if !defined(PLATFORM_VITA)
        StopSound(port.sound);
        SetSoundVolume(port.sound, port.volume);
        PlaySound(port.sound);
#endif
        const double durationSeconds =
            static_cast<double>(port.frameCount) /
            static_cast<double>(port.sampleRate);
        port.deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds(
                static_cast<int64_t>(durationSeconds * 1000000.0));
        port.playing = true;

        if (g386VoiceTraceEnabled())
        {
            std::fprintf(stderr,
                         "[G386:voice] play port=%u file=\"%s\" duration=%.3f\n",
                         portIndex, port.filename.c_str(), durationSeconds);
        }
    }

    uint32_t g386ReadArg0(const uint8_t *sendData, uint32_t sendSize)
    {
        return g386ReadU32(sendData, sendSize, 0u);
    }
}

bool dc2G386HandleVoiceRpc(uint32_t rpcNum,
                           const uint8_t *sendData, uint32_t sendSize,
                           uint32_t &replyWord)
{
    if (g386VoiceDisabled())
    {
        return false;
    }

    const uint32_t family = rpcNum & 0xFFF0u;
    const uint32_t portIndex = rpcNum & 0x000Fu;
    if (portIndex >= g_g386VoicePorts.size())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_g386VoiceMutex);
    replyWord = 0u;

    switch (family)
    {
    case 0x0010u:
    case 0x0030u:
    case 0x0070u:
        g386StopPortLocked(g_g386VoicePorts[portIndex], true);
        break;

    case 0x0040u:
        // Standby commits the open stream on the IOP. Loading already completed
        // synchronously in the 0x8020 open command.
        break;

    case 0x0050u:
        g386PlayPortLocked(portIndex);
        break;

    case 0x0060u:
        g386StopPortLocked(g_g386VoicePorts[portIndex], false);
        break;

    case 0x0080u:
    {
        const uint32_t packedVolume = g386ReadArg0(sendData, sendSize);
        if (packedVolume != 0u)
        {
            g386SetVolumeLocked(portIndex, packedVolume);
        }
        break;
    }

    case 0x8000u:
    case 0x80C0u:
    case 0x80D0u:
        // Stream buffer/standby attributes. The host clip is already resident.
        break;

    case 0x8020u:
    {
        char filename[33]{};
        const size_t bytesToCopy =
            std::min<size_t>(32u, static_cast<size_t>(sendSize));
        if (sendData != nullptr && bytesToCopy > 0u)
        {
            std::memcpy(filename, sendData, bytesToCopy);
        }
        if (!g386OpenPortLocked(portIndex, filename))
        {
            replyWord = 0xFFFFFFFFu;
        }
        break;
    }

    case 0x80B0u:
        g386RefreshPortLocked(portIndex);
        replyWord = g_g386VoicePorts[portIndex].playing
            ? kG386StatePlaying
            : 0u;
        break;

    case 0x80E0u:
        g386RefreshPortLocked(portIndex);
        replyWord = g_g386VoicePorts[portIndex].playing
            ? 0x10001000u
            : 0u;
        break;

    case 0x80F0u:
        for (G386VoicePort &port : g_g386VoicePorts)
        {
            g386StopPortLocked(port, true);
        }
        break;

    default:
        if (g386VoiceTraceEnabled())
        {
            static uint32_t unknownCount = 0u;
            if (unknownCount++ < 32u)
            {
                std::fprintf(stderr,
                             "[G386:voice] unhandled rpc=0x%x family=0x%x port=%u\n",
                             rpcNum, family, portIndex);
            }
        }
        break;
    }

    return true;
}
