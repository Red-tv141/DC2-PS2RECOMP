        bool g384MpegAudioDisabled()
        {
            static const bool disabled = [] {
                const char *dc2 = std::getenv("DC2_G384_NO_MPEG_AUDIO");
                const char *generic = std::getenv("PS2X_NO_MPEG_AUDIO");
                return (dc2 && dc2[0] != '\0' && std::strcmp(dc2, "0") != 0) ||
                       (generic && generic[0] != '\0' && std::strcmp(generic, "0") != 0);
            }();
            return disabled;
        }

        bool g384MpegAudioTraceEnabled()
        {
            static const bool enabled = [] {
                const char *dc2 = std::getenv("DC2_G384_MPEG_AUDIO_TRACE");
                const char *generic = std::getenv("PS2X_MPEG_AUDIO_TRACE");
                return (dc2 && dc2[0] != '\0' && std::strcmp(dc2, "0") != 0) ||
                       (generic && generic[0] != '\0' && std::strcmp(generic, "0") != 0);
            }();
            return enabled;
        }

        uint32_t g384ReadLe32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8u) |
                   (static_cast<uint32_t>(p[2]) << 16u) |
                   (static_cast<uint32_t>(p[3]) << 24u);
        }

        // PS2 PSS private-stream audio contains a Sony Audio Stream (SShd/SSbd)
        // payload. PCM channels are stored in interleave-sized planar blocks rather
        // than normal LRLR sample order, so feed the demuxed payload through this
        // small streaming deinterleaver before handing it to raylib.
        class MpegPcmAudioStream
        {
        public:
            ~MpegPcmAudioStream()
            {
                if (g384MpegAudioTraceEnabled() && m_headerReady)
                {
                    std::cerr << "[G384:mpeg-audio] close body=" << m_bodyBytesAccepted
                              << "/" << m_bodyBytesExpected
                              << " decodedFrames=" << m_decodedFrames
                              << " submittedFrames=" << m_submittedFrames
                              << " pendingFrames=" << pendingFrames()
                              << std::endl;
                }
                unload();
            }

            MpegPcmAudioStream() = default;
            MpegPcmAudioStream(const MpegPcmAudioStream &) = delete;
            MpegPcmAudioStream &operator=(const MpegPcmAudioStream &) = delete;

            void feedPssPayload(const uint8_t *data, size_t size)
            {
                if (g384MpegAudioDisabled() || m_unsupported || !data || size == 0u)
                {
                    return;
                }

                // Sony PSS private_stream_1 packets prepend a four-byte substream
                // selector (DC2 uses ff a0 00 00). It is PES framing, not ADS data.
                if (size >= 4u && data[0] == 0xFFu && (data[1] & 0xF0u) == 0xA0u)
                {
                    data += 4u;
                    size -= 4u;
                }
                if (size == 0u)
                {
                    return;
                }

                if (!m_headerReady)
                {
                    m_headerBytes.insert(m_headerBytes.end(), data, data + size);
                    if (!parseHeader())
                    {
                        return;
                    }

                    // parseHeader leaves only bytes after SSbd's size field here.
                    data = m_headerBytes.data();
                    size = m_headerBytes.size();
                    if (size != 0u)
                    {
                        consumeBody(data, size);
                    }
                    m_headerBytes.clear();
                }
                else
                {
                    consumeBody(data, size);
                }

                pump();
            }

            void finish()
            {
                m_finished = true;
                pump();
                if (g384MpegAudioTraceEnabled() && m_headerReady)
                {
                    std::cerr << "[G384:mpeg-audio] eof body=" << m_bodyBytesAccepted
                              << "/" << m_bodyBytesExpected
                              << " decodedFrames=" << m_decodedFrames
                              << " submittedFrames=" << m_submittedFrames
                              << " pendingFrames=" << pendingFrames()
                              << std::endl;
                }
            }

            void pump()
            {
#if defined(PLATFORM_VITA)
                return;
#else
                if (!m_streamLoaded)
                {
                    return;
                }

                uint32_t uploads = 0u;
                while (pendingFrames() >= kStreamFrames &&
                       IsAudioStreamProcessed(m_stream) &&
                       uploads < 2u)
                {
                    UpdateAudioStream(
                        m_stream,
                        m_pcm.data() + static_cast<std::ptrdiff_t>(m_pcmReadSamples),
                        static_cast<int>(kStreamFrames));
                    m_pcmReadSamples += kStreamFrames * m_channels;
                    m_submittedFrames += kStreamFrames;
                    ++uploads;
                    compactPcm();
                }

                if (m_finished && pendingFrames() != 0u &&
                    pendingFrames() < kStreamFrames &&
                    IsAudioStreamProcessed(m_stream))
                {
                    std::vector<int16_t> tail(kStreamFrames * m_channels, 0);
                    const size_t samples = m_pcm.size() - m_pcmReadSamples;
                    std::memcpy(tail.data(),
                                m_pcm.data() + static_cast<std::ptrdiff_t>(m_pcmReadSamples),
                                samples * sizeof(int16_t));
                    UpdateAudioStream(m_stream, tail.data(), static_cast<int>(kStreamFrames));
                    m_submittedFrames += samples / m_channels;
                    m_pcmReadSamples += samples;
                    compactPcm();
                    ++uploads;
                }

                if (!m_streamStarted && m_submittedFrames != 0u)
                {
                    PlayAudioStream(m_stream);
                    m_streamStarted = true;
                    if (g384MpegAudioTraceEnabled())
                    {
                        std::cerr << "[G384:mpeg-audio] play rate=" << m_sampleRate
                                  << " channels=" << m_channels
                                  << " queuedFrames=" << pendingFrames()
                                  << std::endl;
                    }
                }

                if (g384MpegAudioTraceEnabled() &&
                    m_submittedFrames >= m_nextProgressFrame)
                {
                    std::cerr << "[G384:mpeg-audio] progress submittedFrames="
                              << m_submittedFrames
                              << " decodedFrames=" << m_decodedFrames
                              << " pendingFrames=" << pendingFrames()
                              << std::endl;
                    m_nextProgressFrame =
                        m_submittedFrames + static_cast<uint64_t>(m_sampleRate) * 5u;
                }
#endif
            }

        private:
            static constexpr uint32_t kStreamFrames = 4096u;

            bool parseHeader()
            {
                static constexpr std::array<uint8_t, 4> kSShd = {'S', 'S', 'h', 'd'};
                auto magic = std::search(
                    m_headerBytes.begin(), m_headerBytes.end(),
                    kSShd.begin(), kSShd.end());
                if (magic == m_headerBytes.end())
                {
                    // Preserve a possible split magic while bounding non-ADS streams.
                    if (m_headerBytes.size() > 3u)
                    {
                        m_headerBytes.erase(
                            m_headerBytes.begin(),
                            m_headerBytes.end() - 3);
                    }
                    return false;
                }

                if (magic != m_headerBytes.begin())
                {
                    m_headerBytes.erase(m_headerBytes.begin(), magic);
                }
                if (m_headerBytes.size() < 8u)
                {
                    return false;
                }

                const uint32_t sshdSize = g384ReadLe32(m_headerBytes.data() + 4u);
                const size_t ssbdOffset = 8u + static_cast<size_t>(sshdSize);
                const size_t bodyOffset = ssbdOffset + 8u;
                if (sshdSize < 0x18u || sshdSize > 0x100u)
                {
                    markUnsupported("bad SShd size");
                    return false;
                }
                if (m_headerBytes.size() < bodyOffset)
                {
                    return false;
                }
                if (std::memcmp(m_headerBytes.data() + ssbdOffset, "SSbd", 4u) != 0)
                {
                    markUnsupported("missing SSbd");
                    return false;
                }

                m_codec = g384ReadLe32(m_headerBytes.data() + 0x08u);
                m_sampleRate = g384ReadLe32(m_headerBytes.data() + 0x0Cu);
                m_channels = g384ReadLe32(m_headerBytes.data() + 0x10u);
                m_interleaveBytesPerChannel = g384ReadLe32(m_headerBytes.data() + 0x14u);
                m_bodyBytesExpected = g384ReadLe32(m_headerBytes.data() + ssbdOffset + 4u);

                const bool valid =
                    m_codec == 1u &&
                    (m_channels == 1u || m_channels == 2u) &&
                    m_sampleRate >= 8000u && m_sampleRate <= 192000u &&
                    m_interleaveBytesPerChannel >= 2u &&
                    m_interleaveBytesPerChannel <= (1u << 20u) &&
                    (m_interleaveBytesPerChannel & 1u) == 0u;
                if (!valid)
                {
                    markUnsupported("unsupported ADS format");
                    return false;
                }

                m_headerReady = true;
                m_headerBytes.erase(
                    m_headerBytes.begin(),
                    m_headerBytes.begin() + static_cast<std::ptrdiff_t>(bodyOffset));

                if (g384MpegAudioTraceEnabled())
                {
                    std::cerr << "[G384:mpeg-audio] header codec=" << m_codec
                              << " rate=" << m_sampleRate
                              << " channels=" << m_channels
                              << " interleave=" << m_interleaveBytesPerChannel
                              << " body=" << m_bodyBytesExpected
                              << std::endl;
                }

#if !defined(PLATFORM_VITA)
                if (IsAudioDeviceReady())
                {
                    SetAudioStreamBufferSizeDefault(static_cast<int>(kStreamFrames));
                    m_stream = LoadAudioStream(m_sampleRate, 16u, m_channels);
                    m_streamLoaded = (m_stream.buffer != nullptr);
                }
                if (!m_streamLoaded && g384MpegAudioTraceEnabled())
                {
                    std::cerr << "[G384:mpeg-audio] host audio stream unavailable" << std::endl;
                }
#endif
                return true;
            }

            void markUnsupported(const char *reason)
            {
                m_unsupported = true;
                if (g384MpegAudioTraceEnabled())
                {
                    std::cerr << "[G384:mpeg-audio] " << reason << std::endl;
                }
            }

            void consumeBody(const uint8_t *data, size_t size)
            {
                if (!data || size == 0u || m_bodyBytesAccepted >= m_bodyBytesExpected)
                {
                    return;
                }

                const size_t remaining =
                    static_cast<size_t>(m_bodyBytesExpected - m_bodyBytesAccepted);
                const size_t accepted = std::min(size, remaining);
                m_interleaveBytes.insert(
                    m_interleaveBytes.end(), data, data + accepted);
                m_bodyBytesAccepted += static_cast<uint32_t>(accepted);

                const size_t blockBytes =
                    static_cast<size_t>(m_interleaveBytesPerChannel) * m_channels;
                while (m_interleaveBytes.size() - m_interleaveReadBytes >= blockBytes)
                {
                    const uint8_t *block =
                        m_interleaveBytes.data() +
                        static_cast<std::ptrdiff_t>(m_interleaveReadBytes);
                    const size_t frames =
                        static_cast<size_t>(m_interleaveBytesPerChannel) / sizeof(int16_t);
                    const size_t oldSize = m_pcm.size();
                    m_pcm.resize(oldSize + frames * m_channels);
                    int16_t *dst = m_pcm.data() + static_cast<std::ptrdiff_t>(oldSize);

                    for (size_t frame = 0u; frame < frames; ++frame)
                    {
                        for (uint32_t channel = 0u; channel < m_channels; ++channel)
                        {
                            const size_t src =
                                static_cast<size_t>(channel) * m_interleaveBytesPerChannel +
                                frame * sizeof(int16_t);
                            const uint16_t bits =
                                static_cast<uint16_t>(block[src]) |
                                (static_cast<uint16_t>(block[src + 1u]) << 8u);
                            *dst++ = static_cast<int16_t>(bits);
                        }
                    }

                    m_interleaveReadBytes += blockBytes;
                    m_decodedFrames += frames;
                }
                compactInterleave();
            }

            size_t pendingFrames() const
            {
                if (m_channels == 0u || m_pcmReadSamples >= m_pcm.size())
                {
                    return 0u;
                }
                return (m_pcm.size() - m_pcmReadSamples) / m_channels;
            }

            void compactInterleave()
            {
                if (m_interleaveReadBytes == 0u)
                {
                    return;
                }
                if (m_interleaveReadBytes == m_interleaveBytes.size())
                {
                    m_interleaveBytes.clear();
                    m_interleaveReadBytes = 0u;
                }
                else if (m_interleaveReadBytes >= (64u * 1024u))
                {
                    m_interleaveBytes.erase(
                        m_interleaveBytes.begin(),
                        m_interleaveBytes.begin() +
                            static_cast<std::ptrdiff_t>(m_interleaveReadBytes));
                    m_interleaveReadBytes = 0u;
                }
            }

            void compactPcm()
            {
                if (m_pcmReadSamples == 0u)
                {
                    return;
                }
                if (m_pcmReadSamples == m_pcm.size())
                {
                    m_pcm.clear();
                    m_pcmReadSamples = 0u;
                }
                else if (m_pcmReadSamples >= (64u * 1024u) &&
                         m_pcmReadSamples * 2u >= m_pcm.size())
                {
                    m_pcm.erase(
                        m_pcm.begin(),
                        m_pcm.begin() +
                            static_cast<std::ptrdiff_t>(m_pcmReadSamples));
                    m_pcmReadSamples = 0u;
                }
            }

            void unload()
            {
#if !defined(PLATFORM_VITA)
                if (m_streamLoaded && IsAudioDeviceReady())
                {
                    StopAudioStream(m_stream);
                    UnloadAudioStream(m_stream);
                }
                m_stream = {};
#endif
                m_streamLoaded = false;
                m_streamStarted = false;
            }

            uint32_t m_codec = 0u;
            uint32_t m_sampleRate = 0u;
            uint32_t m_channels = 0u;
            uint32_t m_interleaveBytesPerChannel = 0u;
            uint32_t m_bodyBytesExpected = 0u;
            uint32_t m_bodyBytesAccepted = 0u;
            bool m_headerReady = false;
            bool m_unsupported = false;
            bool m_finished = false;
            bool m_streamLoaded = false;
            bool m_streamStarted = false;
            AudioStream m_stream{};
            std::vector<uint8_t> m_headerBytes;
            std::vector<uint8_t> m_interleaveBytes;
            size_t m_interleaveReadBytes = 0u;
            std::vector<int16_t> m_pcm;
            size_t m_pcmReadSamples = 0u;
            uint64_t m_decodedFrames = 0u;
            uint64_t m_submittedFrames = 0u;
            uint64_t m_nextProgressFrame = 1u;
        };
