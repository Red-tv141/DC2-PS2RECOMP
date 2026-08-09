// G544 - default-off chained VU1 semantic replay and ordered packet-ring oracle.
//
// G543 proved each captured kick independently. This phase submits the entire corpus in one GPU
// invocation, retains VU state/pipeline/code/data between records, applies only captured incoming
// writes, and publishes every XGKICK into bounded monotonic producer rings. CPU corpus bytes remain
// authoritative; one final readback checks every intermediate boundary and the persistent tail.

#include "g544_vu_gpu_chain_shader.inc"

constexpr uint32_t G544_STATUS_MAGIC = 0x34344753u; // little-endian "SG44"
constexpr uint32_t G544_STATUS_WORDS = 8u;
constexpr uint32_t G544_META_MAGIC = 0x41523434u; // little-endian "44RA"
constexpr uint32_t G544_META_WORDS = 12u;
constexpr uint32_t G544_WORKSPACE_WORDS = 158u + 161u + 4096u + 4096u;

static bool g544NextPow2(uint64_t need, uint32_t floor, uint32_t &out)
{
    uint64_t value = 1u;
    need = std::max<uint64_t>(need, floor);
    while (value < need && value <= 0x80000000ull)
        value <<= 1u;
    if (value < need || value > 0xffffffffull)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

static bool g544LinkProgram(GLFns &f, GLuint &program, std::string &why)
{
    const std::string source = g544ChainShaderSource(why);
    if (source.empty())
        return false;

    GLuint cs = 0u;
    std::string log;
    if (!g158CompileShader(f, GL541_COMPUTE_SHADER, source.c_str(), cs, log))
    {
        why = "compute compile failed: " + log;
        return false;
    }
    program = f.glCreateProgram_();
    f.glAttachShader_(program, cs);
    f.glLinkProgram_(program);
    GLint linked = 0;
    f.glGetProgramiv_(program, GL158_LINK_STATUS, &linked);
    f.glDeleteShader_(cs);
    if (!linked)
    {
        GLint len = 0;
        f.glGetProgramiv_(program, GL158_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(len > 0 ? static_cast<size_t>(len) : 1u, 0);
        f.glGetProgramInfoLog_(program, static_cast<GLsizei>(buf.size()), nullptr, buf.data());
        why = "compute link failed: " + std::string(buf.data());
        return false;
    }
    return true;
}

static bool g544CompareAt(const G542AbiCorpus &corpus,
                          const std::vector<uint32_t> &output,
                          uint32_t record, const char *section,
                          uint32_t expectedByteOffset, uint32_t actualByteOffset,
                          uint32_t byteCount, G543Mismatch &mismatch)
{
    const uint64_t expectedEnd = static_cast<uint64_t>(expectedByteOffset) + byteCount;
    const uint64_t actualEnd = static_cast<uint64_t>(actualByteOffset) + byteCount;
    const uint64_t payloadBytes = static_cast<uint64_t>(corpus.payload.size()) * 4u;
    const uint64_t outputBytes = static_cast<uint64_t>(output.size()) * 4u;
    if (expectedEnd > payloadBytes || actualEnd > outputBytes)
    {
        mismatch.section = section;
        mismatch.record = record;
        mismatch.byte = 0u;
        mismatch.absoluteByte = actualByteOffset;
        return false;
    }

    const auto *expected = reinterpret_cast<const uint8_t *>(corpus.payload.data());
    const auto *actual = reinterpret_cast<const uint8_t *>(output.data());
    for (uint32_t b = 0u; b < byteCount; ++b)
    {
        if (expected[expectedByteOffset + b] == actual[actualByteOffset + b])
            continue;
        mismatch.section = section;
        mismatch.record = record;
        mismatch.byte = b;
        mismatch.absoluteByte = actualByteOffset + b;
        mismatch.expected = expected[expectedByteOffset + b];
        mismatch.actual = actual[actualByteOffset + b];
        const uint32_t expectedWordByte = (expectedByteOffset + b) & ~3u;
        const uint32_t actualWordByte = (actualByteOffset + b) & ~3u;
        mismatch.expectedWord = g542ReadU32(expected + expectedWordByte);
        mismatch.actualWord = g542ReadU32(actual + actualWordByte);
        return false;
    }
    return true;
}

static void g544PrintMismatch(const G543Mismatch &mismatch)
{
    std::fprintf(stderr,
                 "[G544:chain] FAIL record=%u section=%s byte=%u actualByte=%u expected=%02x actual=%02x expectedWord=%08x actualWord=%08x; CPU path unchanged\n",
                 mismatch.record, mismatch.section, mismatch.byte, mismatch.absoluteByte,
                 static_cast<unsigned>(mismatch.expected),
                 static_cast<unsigned>(mismatch.actual),
                 mismatch.expectedWord, mismatch.actualWord);
}

static bool g544RunChainReplay(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr)
    {
        std::fprintf(stderr,
                     "[G544:chain] FAIL compute/image entry points unavailable; CPU path unchanged\n");
        return false;
    }

    const char *envPath = std::getenv("DC2_G544_CORPUS_PATH");
    if (envPath == nullptr || envPath[0] == '\0')
        envPath = std::getenv("DC2_G543_CORPUS_PATH");
    if (envPath == nullptr || envPath[0] == '\0')
        envPath = std::getenv("DC2_G542_CORPUS_PATH");
    const std::filesystem::path path = envPath && envPath[0] != '\0'
        ? std::filesystem::path(envPath)
        : std::filesystem::path("captures/g541_vu1_corpus.bin");

    G542AbiCorpus corpus;
    std::string why;
    if (!g542LoadAbiCorpus(path, corpus, why) || !g543ValidateLayout(corpus, why))
    {
        std::fprintf(stderr, "[G544:chain] FAIL path='%s': %s; CPU path unchanged\n",
                     path.string().c_str(), why.c_str());
        return false;
    }
    if (corpus.records < 7u)
    {
        std::fprintf(stderr,
                     "[G544:chain] FAIL path='%s': chained control overlay needs at least 7 records; CPU path unchanged\n",
                     path.string().c_str());
        return false;
    }

    uint64_t totalPackets64 = 0u;
    uint64_t totalPacketBytes64 = 0u;
    uint64_t totalWrites = 0u;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        const uint32_t *cmd = corpus.commands.data() +
                              static_cast<size_t>(r) * G542_ABI_WORDS;
        totalPackets64 += cmd[27];
        totalPacketBytes64 += cmd[29];
        totalWrites += cmd[23];
    }
    if (totalPackets64 > 0xfffffffeull || totalPacketBytes64 > 0xfffffffeull)
    {
        std::fprintf(stderr,
                     "[G544:chain] FAIL corpus packet totals exceed 32-bit ring ABI; CPU path unchanged\n");
        return false;
    }

    uint32_t packetCapacity = 0u;
    uint32_t byteCapacity = 0u;
    if (!g544NextPow2(totalPackets64 + 1u, 64u, packetCapacity) ||
        !g544NextPow2(totalPacketBytes64 + 1u, 1024u, byteCapacity))
    {
        std::fprintf(stderr,
                     "[G544:chain] FAIL corpus exceeds bounded packet-ring capacity; CPU path unchanged\n");
        return false;
    }

    const uint64_t statusWord64 = corpus.payload.size();
    const uint64_t ringDescWord64 = statusWord64 +
                                    static_cast<uint64_t>(corpus.records) * G544_STATUS_WORDS;
    const uint64_t ringBytesWord64 = ringDescWord64 +
                                     static_cast<uint64_t>(packetCapacity) * 2u;
    const uint64_t metaWord64 = ringBytesWord64 +
                                (static_cast<uint64_t>(byteCapacity) + 3u) / 4u;
    const uint64_t workspaceWord64 = metaWord64 + G544_META_WORDS;
    const uint64_t outputWords64 = workspaceWord64 + G544_WORKSPACE_WORDS;
    if (outputWords64 > 0xffffffffull || ringDescWord64 * 4u > 0xffffffffull ||
        ringBytesWord64 * 4u > 0xffffffffull)
    {
        std::fprintf(stderr,
                     "[G544:chain] FAIL output layout exceeds 32-bit image ABI; CPU path unchanged\n");
        return false;
    }

    const uint32_t statusWord = static_cast<uint32_t>(statusWord64);
    const uint32_t ringDescWord = static_cast<uint32_t>(ringDescWord64);
    const uint32_t ringBytesWord = static_cast<uint32_t>(ringBytesWord64);
    const uint32_t metaWord = static_cast<uint32_t>(metaWord64);
    const uint32_t workspaceWord = static_cast<uint32_t>(workspaceWord64);
    const uint32_t outputWords = static_cast<uint32_t>(outputWords64);

    for (uint32_t r = 0u; r < corpus.records; ++r)
        corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 31u] =
            statusWord + r * G544_STATUS_WORDS;
    corpus.commands[0u * G542_ABI_WORDS + 30u] = corpus.records;
    corpus.commands[1u * G542_ABI_WORDS + 30u] = ringDescWord * 4u;
    corpus.commands[2u * G542_ABI_WORDS + 30u] = ringBytesWord * 4u;
    corpus.commands[3u * G542_ABI_WORDS + 30u] = packetCapacity;
    corpus.commands[4u * G542_ABI_WORDS + 30u] = byteCapacity;
    corpus.commands[5u * G542_ABI_WORDS + 30u] = metaWord;
    corpus.commands[6u * G542_ABI_WORDS + 30u] = workspaceWord;

    G542GlObjects gl;
    gl.f = &f;
    if (!g544LinkProgram(f, gl.program, why))
    {
        std::fprintf(stderr, "[G544:chain] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {}
    uint32_t commandHeight = 0u, payloadHeight = 0u, outputHeight = 0u;
    std::vector<uint32_t> outputInit(outputWords, 0xa5a5a5a5u);
    if (!g542CreateImage(gl.textures[0], corpus.commands, commandHeight, why) ||
        !g542CreateImage(gl.textures[1], corpus.payload, payloadHeight, why) ||
        !g542CreateImage(gl.textures[2], outputInit, outputHeight, why))
    {
        std::fprintf(stderr, "[G544:chain] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    f.glUseProgram_(gl.program);
    e.glBindImageTexture_(2u, gl.textures[0], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(3u, gl.textures[1], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(4u, gl.textures[2], 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
    e.glDispatchCompute_(1u, 1u, 1u);
    e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL178_TEXTURE_UPDATE_BARRIER_BIT);

    std::vector<uint32_t> output(static_cast<size_t>(G542_IMAGE_WIDTH) * outputHeight);
    glBindTexture(GL_TEXTURE_2D, gl.textures[2]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, output.data());
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        std::fprintf(stderr, "[G544:chain] FAIL readback gl=0x%x; CPU path unchanged\n",
                     static_cast<unsigned>(err));
        return false;
    }

    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        const uint32_t *cmd = corpus.commands.data() +
                              static_cast<size_t>(r) * G542_ABI_WORDS;
        const size_t s = cmd[31];
        if (s + G544_STATUS_WORDS > output.size() || output[s] != G544_STATUS_MAGIC ||
            output[s + 1u] != 0u || output[s + 4u] != cmd[10] ||
            output[s + 5u] != cmd[12] || output[s + 6u] != cmd[27] ||
            output[s + 7u] != cmd[29])
        {
            const uint32_t magic = s < output.size() ? output[s] : 0u;
            const uint32_t flags = s + 1u < output.size() ? output[s + 1u] : 0xffffffffu;
            const uint32_t pc = s + 2u < output.size() ? output[s + 2u] : 0xffffffffu;
            const uint32_t word = s + 3u < output.size() ? output[s + 3u] : 0u;
            const uint32_t cycles = s + 4u < output.size() ? output[s + 4u] : 0u;
            const uint32_t stop = s + 5u < output.size() ? output[s + 5u] : 0u;
            const uint32_t packets = s + 6u < output.size() ? output[s + 6u] : 0u;
            const uint32_t bytes = s + 7u < output.size() ? output[s + 7u] : 0u;
            std::fprintf(stderr,
                         "[G544:chain] FAIL record=%u magic=%08x flags=%08x pc=%04x word=%08x cycles=%u/%u stop=%u/%u packets=%u/%u bytes=%u/%u; CPU path unchanged\n",
                         r, magic, flags, pc, word, cycles, cmd[10], stop, cmd[12],
                         packets, cmd[27], bytes, cmd[29]);
            return false;
        }

        G543Mismatch mismatch;
        if (!g544CompareAt(corpus, output, r, "postState", cmd[16], cmd[16], 632u,
                           mismatch) ||
            !g544CompareAt(corpus, output, r, "postPipe", cmd[18], cmd[18], 644u,
                           mismatch) ||
            !g544CompareAt(corpus, output, r, "postData", cmd[21], cmd[21], 16384u,
                           mismatch))
        {
            g544PrintMismatch(mismatch);
            return false;
        }
    }

    uint32_t actualDescByte = ringDescWord * 4u;
    uint32_t actualPacketByte = ringBytesWord * 4u;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        const uint32_t *cmd = corpus.commands.data() +
                              static_cast<size_t>(r) * G542_ABI_WORDS;
        G543Mismatch mismatch;
        const uint32_t descBytes = cmd[27] * 8u;
        if (!g544CompareAt(corpus, output, r, "ringDesc", cmd[26], actualDescByte,
                           descBytes, mismatch) ||
            !g544CompareAt(corpus, output, r, "ringBytes", cmd[28], actualPacketByte,
                           cmd[29], mismatch))
        {
            g544PrintMismatch(mismatch);
            return false;
        }
        actualDescByte += descBytes;
        actualPacketByte += cmd[29];
    }

    const uint32_t expectedMeta[G544_META_WORDS] = {
        G544_META_MAGIC, 1u, corpus.records, packetCapacity, byteCapacity,
        static_cast<uint32_t>(totalPackets64), static_cast<uint32_t>(totalPacketBytes64),
        0u, 0u, 0u, workspaceWord, G544_WORKSPACE_WORDS};
    for (uint32_t w = 0u; w < G544_META_WORDS; ++w)
    {
        if (output[metaWord + w] == expectedMeta[w])
            continue;
        std::fprintf(stderr,
                     "[G544:chain] FAIL metadataWord=%u expected=%08x actual=%08x; CPU path unchanged\n",
                     w, expectedMeta[w], output[metaWord + w]);
        return false;
    }

    const uint32_t *last = corpus.commands.data() +
                           static_cast<size_t>(corpus.records - 1u) * G542_ABI_WORDS;
    G543Mismatch mismatch;
    uint32_t workspaceByte = workspaceWord * 4u;
    if (!g544CompareAt(corpus, output, corpus.records - 1u, "tailState", last[16],
                       workspaceByte, 632u, mismatch) ||
        !g544CompareAt(corpus, output, corpus.records - 1u, "tailPipe", last[18],
                       workspaceByte + 632u, 644u, mismatch) ||
        !g544CompareAt(corpus, output, corpus.records - 1u, "tailCode", last[19],
                       workspaceByte + 632u + 644u, 16384u, mismatch) ||
        !g544CompareAt(corpus, output, corpus.records - 1u, "tailData", last[21],
                       workspaceByte + 632u + 644u + 16384u, 16384u, mismatch))
    {
        g544PrintMismatch(mismatch);
        return false;
    }

    std::fprintf(stderr,
                 "[G544:chain] PASS records=%u writes=%llu packets=%u bytes=%u packetCap=%u byteCap=%u oneDispatch=1 oneReadback=1 persistentStatePipeCodeData=1 CPU-authoritative=1\n",
                 corpus.records, static_cast<unsigned long long>(totalWrites),
                 static_cast<uint32_t>(totalPackets64),
                 static_cast<uint32_t>(totalPacketBytes64), packetCapacity, byteCapacity);
    return true;
}
