// G542 - versioned, padding-free VU1 GPU command ABI and offline corpus transport gate.
//
// This phase deliberately proves only the CPU-file -> GPU-image -> CPU-readback boundary. It does
// NOT execute a VU instruction and can never become authoritative. Each corpus record is converted
// to a fixed 32-word little-endian command descriptor; the original padding-free payload is packed
// into an R32UI image. One compute invocation per record walks its descriptor, copies every payload
// word into a separate image, and publishes a word hash/status. One bounded readback then compares
// every word. Later semantic phases reuse this exact descriptor and replace only the copy loop.

constexpr uint32_t G542_ABI_MAGIC = 0x32435556u; // little-endian "VUC2"
constexpr uint32_t G542_ABI_VERSION = 1u;
constexpr uint32_t G542_ABI_WORDS = 32u;
constexpr uint32_t G542_STATUS_MAGIC = 0x4b4f3234u; // little-endian "42OK"
constexpr uint32_t G542_IMAGE_WIDTH = 4096u;
constexpr GLenum GL542_READ_ONLY = 0x88B8;
constexpr GLenum GL542_WRITE_ONLY = 0x88B9;

struct G542AbiCorpus
{
    std::vector<uint32_t> commands;
    std::vector<uint32_t> payload;
    std::vector<uint32_t> recordHashes;
    uint64_t payloadBytes = 0u;
    uint32_t records = 0u;
};

static uint32_t g542ReadU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

static uint64_t g542ReadU64(const uint8_t *p)
{
    return static_cast<uint64_t>(g542ReadU32(p)) |
           (static_cast<uint64_t>(g542ReadU32(p + 4u)) << 32u);
}

static uint64_t g542FnvBytes(const uint8_t *p, size_t bytes)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0u; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t g542FnvWords(const uint8_t *p, size_t bytes)
{
    uint32_t h = 2166136261u;
    for (size_t at = 0u; at < bytes; at += 4u)
    {
        uint32_t w = 0u;
        const size_t remain = std::min<size_t>(4u, bytes - at);
        for (size_t b = 0u; b < remain; ++b)
            w |= static_cast<uint32_t>(p[at + b]) << (8u * static_cast<uint32_t>(b));
        h ^= w;
        h *= 16777619u;
    }
    return h;
}

static bool g542LoadAbiCorpus(const std::filesystem::path &path, G542AbiCorpus &out,
                              std::string &why)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        why = "could not open corpus";
        return false;
    }
    const std::streamoff end = in.tellg();
    if (end < 32 || end > static_cast<std::streamoff>(128ull * 1024ull * 1024ull))
    {
        why = "corpus size outside 32 B..128 MiB";
        return false;
    }
    std::vector<uint8_t> file(static_cast<size_t>(end));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char *>(file.data()), static_cast<std::streamsize>(end)))
    {
        why = "short corpus read";
        return false;
    }

    static const uint8_t fileMagic[8] = {'D','C','2','V','U','1','K',0};
    if (std::memcmp(file.data(), fileMagic, sizeof(fileMagic)) != 0 ||
        g542ReadU32(file.data() + 8u) != 1u ||
        g542ReadU32(file.data() + 12u) != 0x01020304u ||
        g542ReadU32(file.data() + 28u) != 32u)
    {
        why = "bad G541 file header";
        return false;
    }

    const uint32_t codeSize = g542ReadU32(file.data() + 16u);
    const uint32_t dataSize = g542ReadU32(file.data() + 20u);
    const uint32_t stateSize = g542ReadU32(file.data() + 24u);
    std::vector<uint8_t> packed;
    size_t cursor = 32u;
    while (cursor < file.size())
    {
        if (out.records >= 4096u || cursor + 116u > file.size())
        {
            why = "record count/header outside ABI bound";
            return false;
        }
        const uint8_t *r = file.data() + cursor;
        static const uint8_t recMagic[8] = {'G','5','4','1','R','E','C','1'};
        if (std::memcmp(r, recMagic, sizeof(recMagic)) != 0 ||
            g542ReadU32(r + 8u) != 1u || g542ReadU32(r + 12u) != 116u)
        {
            why = "bad record header";
            return false;
        }
        const uint64_t sequence = g542ReadU64(r + 16u);
        const uint64_t payloadSize64 = g542ReadU64(r + 24u);
        const uint64_t checksum = g542ReadU64(r + 32u);
        const uint64_t cycles = g542ReadU64(r + 40u);
        if (sequence != out.records || payloadSize64 > file.size() - cursor - 116u)
        {
            why = "record sequence/payload bound mismatch";
            return false;
        }
        const size_t payloadSize = static_cast<size_t>(payloadSize64);
        const uint8_t *payload = r + 116u;
        if (g542FnvBytes(payload, payloadSize) != checksum)
        {
            why = "record payload checksum mismatch";
            return false;
        }

        const uint32_t flags = g542ReadU32(r + 48u);
        const uint32_t startPc = g542ReadU32(r + 52u);
        const uint32_t itop = g542ReadU32(r + 56u);
        const uint32_t top = g542ReadU32(r + 60u);
        const uint32_t budget = g542ReadU32(r + 64u);
        const uint32_t stopReason = g542ReadU32(r + 68u);
        const uint32_t writeCount = g542ReadU32(r + 72u);
        const uint32_t writeBytes = g542ReadU32(r + 76u);
        const uint32_t packetCount = g542ReadU32(r + 80u);
        const uint32_t packetBytes = g542ReadU32(r + 84u);
        const uint32_t preState = g542ReadU32(r + 88u);
        const uint32_t postState = g542ReadU32(r + 92u);
        const uint32_t prePipe = g542ReadU32(r + 96u);
        const uint32_t postPipe = g542ReadU32(r + 100u);
        const uint32_t recCode = g542ReadU32(r + 104u);
        const uint32_t preData = g542ReadU32(r + 108u);
        const uint32_t postData = g542ReadU32(r + 112u);
        const uint64_t expected = static_cast<uint64_t>(preState) + postState + prePipe +
            postPipe + recCode + preData + postData + static_cast<uint64_t>(writeCount) * 12u +
            writeBytes + static_cast<uint64_t>(packetCount) * 8u + packetBytes;
        if (expected != payloadSize64 || preState != stateSize || postState != stateSize ||
            recCode != codeSize || preData != dataSize || postData != dataSize)
        {
            why = "record semantic layout mismatch";
            return false;
        }

        while ((packed.size() & 3u) != 0u)
            packed.push_back(0u);
        if (packed.size() > 0xffffffffull || payloadSize > 0xffffffffull)
        {
            why = "record payload exceeds 32-bit ABI";
            return false;
        }
        const uint32_t base = static_cast<uint32_t>(packed.size());
        const uint32_t preStateOff = base;
        const uint32_t postStateOff = preStateOff + preState;
        const uint32_t prePipeOff = postStateOff + postState;
        const uint32_t postPipeOff = prePipeOff + prePipe;
        const uint32_t codeOff = postPipeOff + postPipe;
        const uint32_t preDataOff = codeOff + recCode;
        const uint32_t postDataOff = preDataOff + preData;
        const uint32_t writesOff = postDataOff + postData;
        const uint32_t writeBytesOff = writesOff + writeCount * 12u;
        const uint32_t packetDescOff = writeBytesOff + writeBytes;
        const uint32_t packetBytesOff = packetDescOff + packetCount * 8u;
        if (static_cast<uint64_t>(packetBytesOff) + packetBytes !=
            static_cast<uint64_t>(base) + payloadSize)
        {
            why = "record section offsets do not close";
            return false;
        }
        packed.insert(packed.end(), payload, payload + payloadSize);

        const uint32_t hash = g542FnvWords(payload, payloadSize);
        const uint32_t cmd[G542_ABI_WORDS] = {
            G542_ABI_MAGIC, G542_ABI_VERSION, G542_ABI_WORDS, flags,
            static_cast<uint32_t>(sequence), static_cast<uint32_t>(sequence >> 32u),
            startPc, itop, top, budget, static_cast<uint32_t>(cycles),
            static_cast<uint32_t>(cycles >> 32u), stopReason, base,
            static_cast<uint32_t>(payloadSize), preStateOff, postStateOff, prePipeOff,
            postPipeOff, codeOff, preDataOff, postDataOff, writesOff, writeCount,
            writeBytesOff, writeBytes, packetDescOff, packetCount, packetBytesOff, packetBytes,
            hash, 0u}; // word 31 is filled with the shared-output status offset below.
        out.commands.insert(out.commands.end(), cmd, cmd + G542_ABI_WORDS);
        out.recordHashes.push_back(hash);
        out.payloadBytes += payloadSize;
        ++out.records;
        cursor += 116u + payloadSize;
    }
    if (out.records == 0u || cursor != file.size())
    {
        why = "empty/truncated corpus";
        return false;
    }
    while ((packed.size() & 3u) != 0u)
        packed.push_back(0u);
    out.payload.resize(packed.size() / 4u);
    for (size_t i = 0u; i < out.payload.size(); ++i)
        out.payload[i] = g542ReadU32(packed.data() + i * 4u);
    for (uint32_t r = 0u; r < out.records; ++r)
        out.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 31u] =
            static_cast<uint32_t>(out.payload.size() + static_cast<size_t>(r) * 4u);
    return true;
}

struct G542GlObjects
{
    GLFns *f = nullptr;
    GLuint program = 0u;
    GLuint textures[3]{};
    ~G542GlObjects()
    {
        if (textures[0] || textures[1] || textures[2])
            glDeleteTextures(3, textures);
        if (program != 0u && f != nullptr)
            f->glDeleteProgram_(program);
    }
};

static bool g542CreateImage(GLuint &tex, const std::vector<uint32_t> &words,
                            uint32_t &height, std::string &why)
{
    height = static_cast<uint32_t>((words.size() + G542_IMAGE_WIDTH - 1u) / G542_IMAGE_WIDTH);
    if (height == 0u)
        height = 1u;
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (maxSize < static_cast<GLint>(G542_IMAGE_WIDTH) || height > static_cast<uint32_t>(maxSize))
    {
        why = "R32UI image exceeds GL_MAX_TEXTURE_SIZE";
        return false;
    }
    std::vector<uint32_t> padded(static_cast<size_t>(G542_IMAGE_WIDTH) * height, 0u);
    std::copy(words.begin(), words.end(), padded.begin());
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                 static_cast<GLsizei>(G542_IMAGE_WIDTH), static_cast<GLsizei>(height), 0,
                 GL541_RED_INTEGER, GL_UNSIGNED_INT, padded.data());
    if (tex == 0u || glGetError() != GL_NO_ERROR)
    {
        why = "R32UI image allocation failed";
        return false;
    }
    return true;
}

static bool g542LinkProgram(GLFns &f, GLuint &program, std::string &why)
{
    static const char *kCS = R"GLSL(#version 430 core
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(r32ui, binding=2) readonly uniform uimage2D gCommands;
layout(r32ui, binding=3) readonly uniform uimage2D gPayload;
layout(r32ui, binding=4) writeonly uniform uimage2D gOutput;
ivec2 linearCoord(uint i) { return ivec2(int(i & 4095u), int(i >> 12u)); }
uint commandWord(uint i) { return imageLoad(gCommands, linearCoord(i)).r; }
uint payloadWord(uint i) { return imageLoad(gPayload, linearCoord(i)).r; }
void main() {
    uint rec = gl_GlobalInvocationID.x;
    uint c = rec * 32u;
    uint errors = 0u;
    if (commandWord(c + 0u) != 0x32435556u) errors |= 1u;
    if (commandWord(c + 1u) != 1u) errors |= 2u;
    if (commandWord(c + 2u) != 32u) errors |= 4u;
    uint byteOff = commandWord(c + 13u);
    uint byteCount = commandWord(c + 14u);
    if ((byteOff & 3u) != 0u) errors |= 8u;
    uint first = byteOff >> 2u;
    uint count = (byteCount + 3u) >> 2u;
    uint hash = 2166136261u;
    if (errors == 0u) {
        for (uint i = 0u; i < count; ++i) {
            uint w = payloadWord(first + i);
            imageStore(gOutput, linearCoord(first + i), uvec4(w, 0u, 0u, 0u));
            hash = (hash ^ w) * 16777619u;
        }
        if (hash != commandWord(c + 30u)) errors |= 16u;
    }
    uint s = commandWord(c + 31u);
    imageStore(gOutput, linearCoord(s + 0u), uvec4(0x4b4f3234u, 0u, 0u, 0u));
    imageStore(gOutput, linearCoord(s + 1u), uvec4(hash, 0u, 0u, 0u));
    imageStore(gOutput, linearCoord(s + 2u), uvec4(count, 0u, 0u, 0u));
    imageStore(gOutput, linearCoord(s + 3u), uvec4(errors, 0u, 0u, 0u));
}
)GLSL";
    GLuint cs = 0u;
    std::string log;
    if (!g158CompileShader(f, GL541_COMPUTE_SHADER, kCS, cs, log))
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

static bool g542RunAbiReplay(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr)
    {
        std::fprintf(stderr, "[G542:abi] FAIL compute/image entry points unavailable; CPU path unchanged\n");
        return false;
    }
    const char *envPath = std::getenv("DC2_G542_CORPUS_PATH");
    const std::filesystem::path path = envPath && envPath[0] != '\0'
        ? std::filesystem::path(envPath)
        : std::filesystem::path("captures/g541_vu1_corpus.bin");
    G542AbiCorpus corpus;
    std::string why;
    if (!g542LoadAbiCorpus(path, corpus, why))
    {
        std::fprintf(stderr, "[G542:abi] FAIL path='%s': %s; CPU path unchanged\n",
                     path.string().c_str(), why.c_str());
        return false;
    }

    G542GlObjects gl;
    gl.f = &f;
    if (!g542LinkProgram(f, gl.program, why))
    {
        std::fprintf(stderr, "[G542:abi] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }
    while (glGetError() != GL_NO_ERROR) {}
    uint32_t commandHeight = 0u, payloadHeight = 0u, outputHeight = 0u;
    std::vector<uint32_t> outputInit(
        corpus.payload.size() + static_cast<size_t>(corpus.records) * 4u, 0xa5a5a5a5u);
    if (!g542CreateImage(gl.textures[0], corpus.commands, commandHeight, why) ||
        !g542CreateImage(gl.textures[1], corpus.payload, payloadHeight, why) ||
        !g542CreateImage(gl.textures[2], outputInit, outputHeight, why))
    {
        std::fprintf(stderr, "[G542:abi] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    f.glUseProgram_(gl.program);
    e.glBindImageTexture_(2u, gl.textures[0], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(3u, gl.textures[1], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(4u, gl.textures[2], 0, GL_FALSE, 0, GL542_WRITE_ONLY, GL541_R32UI);
    e.glDispatchCompute_(corpus.records, 1u, 1u);
    e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL178_TEXTURE_UPDATE_BARRIER_BIT);

    std::vector<uint32_t> output(static_cast<size_t>(G542_IMAGE_WIDTH) * outputHeight);
    glBindTexture(GL_TEXTURE_2D, gl.textures[2]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, output.data());
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        std::fprintf(stderr, "[G542:abi] FAIL readback gl=0x%x; CPU path unchanged\n",
                     static_cast<unsigned>(err));
        return false;
    }

    size_t badWord = corpus.payload.size();
    for (size_t i = 0u; i < corpus.payload.size(); ++i)
        if (output[i] != corpus.payload[i]) { badWord = i; break; }
    uint32_t badRecord = corpus.records;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        const size_t s = corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 31u];
        const uint32_t bytes = corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 14u];
        if (output[s] != G542_STATUS_MAGIC || output[s + 1u] != corpus.recordHashes[r] ||
            output[s + 2u] != (bytes + 3u) / 4u || output[s + 3u] != 0u)
        {
            badRecord = r;
            break;
        }
    }
    if (badWord != corpus.payload.size() || badRecord != corpus.records)
    {
        std::fprintf(stderr,
                     "[G542:abi] FAIL records=%u badRecord=%u badWord=%llu; CPU path unchanged\n",
                     corpus.records, badRecord,
                     static_cast<unsigned long long>(badWord));
        return false;
    }
    std::fprintf(stderr,
                 "[G542:abi] PASS records=%u commands=%lluB payload=%lluB words=%llu oneReadback=1\n",
                 corpus.records,
                 static_cast<unsigned long long>(corpus.commands.size() * sizeof(uint32_t)),
                 static_cast<unsigned long long>(corpus.payloadBytes),
                 static_cast<unsigned long long>(corpus.payload.size()));
    return true;
}
