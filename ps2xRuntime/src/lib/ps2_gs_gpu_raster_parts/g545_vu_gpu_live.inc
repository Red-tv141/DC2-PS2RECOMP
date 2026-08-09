// G545 - live incremental VU1 GPU shadow transport support.
#include "g545_vu_gpu_live_shader.inc"

constexpr uint32_t G545_LIVE_MAGIC = 0x35435556u; // little-endian "VUC5"
constexpr uint32_t G545_LIVE_WORDS = 32u;
constexpr uint32_t G545_LIVE_STATUS_MAGIC = 0x35343547u; // little-endian "G545"
constexpr uint32_t G545_LIVE_WORKSPACE_WORDS = 16u + 8511u;
constexpr size_t G545_LIVE_MAX_BATCH_BYTES = 2u * 1024u * 1024u;
// NVIDIA/Windows watchdog bound. G544 proved 32 records in one offline invocation, but live kicks
// include uncapped route outliers; 16 retains useful batching with a conservative TDR margin.
constexpr size_t G545_LIVE_MAX_BATCH_COMMANDS = 16u;

struct G545LiveBatch
{
    std::vector<std::vector<uint8_t>> blobs;
    size_t bytes = 0u;
};

static uint32_t g545ReadU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

static bool g545Range(uint32_t offset, uint64_t bytes, uint32_t payloadBytes)
{
    return (offset & 3u) == 0u && static_cast<uint64_t>(offset) + bytes <= payloadBytes;
}

static bool g545ValidateBlob(const std::vector<uint8_t> &blob, std::string &why)
{
    if (blob.size() < G545_LIVE_WORDS * 4u || blob.size() > 2u * 1024u * 1024u ||
        (blob.size() & 3u) != 0u)
    {
        why = "blob size/alignment outside bound";
        return false;
    }
    const uint8_t *h = blob.data();
    const auto w = [&](uint32_t n) { return g545ReadU32(h + n * 4u); };
    const uint32_t payloadBytes = static_cast<uint32_t>(blob.size() - 128u);
    const uint32_t flags = w(3u);
    if (w(0u) != G545_LIVE_MAGIC || w(1u) != 1u || w(2u) != G545_LIVE_WORDS ||
        w(14u) != payloadBytes || w(31u) != payloadBytes || (flags & ~7u) != 0u)
    {
        why = "bad live command header";
        return false;
    }
    if (!g545Range(w(16u), 632u, payloadBytes) ||
        !g545Range(w(18u), 644u, payloadBytes) ||
        !g545Range(w(22u), static_cast<uint64_t>(w(23u)) * 12u, payloadBytes) ||
        !g545Range(w(24u), w(25u), payloadBytes) ||
        !g545Range(w(26u), static_cast<uint64_t>(w(27u)) * 8u, payloadBytes) ||
        !g545Range(w(28u), w(29u), payloadBytes))
    {
        why = "incremental section outside payload";
        return false;
    }
    if ((flags & 2u) != 0u &&
        (!g545Range(w(15u), 632u, payloadBytes) ||
         !g545Range(w(17u), 644u, payloadBytes) ||
         !g545Range(w(19u), 16384u, payloadBytes) ||
         !g545Range(w(20u), 16384u, payloadBytes)))
    {
        why = "seed section outside payload";
        return false;
    }
    if ((flags & 4u) != 0u && !g545Range(w(21u), 16384u, payloadBytes))
    {
        why = "checkpoint section outside payload";
        return false;
    }
    return true;
}

static bool g545PackBatch(const G545LiveBatch &batch, std::vector<uint32_t> &commands,
                          std::vector<uint32_t> &payload, std::string &why)
{
    if (batch.blobs.empty() || batch.blobs.size() > G545_LIVE_MAX_BATCH_COMMANDS)
    {
        why = "empty/oversized batch";
        return false;
    }
    std::vector<uint8_t> packed;
    packed.reserve(batch.bytes);
    commands.reserve(batch.blobs.size() * G545_LIVE_WORDS);
    static const uint32_t offsetWords[] = {15u,16u,17u,18u,19u,20u,21u,22u,24u,26u,28u};
    for (const auto &blob : batch.blobs)
    {
        if (!g545ValidateBlob(blob, why))
            return false;
        while ((packed.size() & 3u) != 0u)
            packed.push_back(0u);
        if (packed.size() > 0xffffffffull)
        {
            why = "packed payload exceeds 32-bit ABI";
            return false;
        }
        const uint32_t base = static_cast<uint32_t>(packed.size());
        uint32_t cmd[G545_LIVE_WORDS];
        for (uint32_t n = 0u; n < G545_LIVE_WORDS; ++n)
            cmd[n] = g545ReadU32(blob.data() + n * 4u);
        cmd[13] = base;
        for (uint32_t n : offsetWords)
        {
            if (cmd[n] > 0xffffffffu - base)
            {
                why = "absolute payload offset overflow";
                return false;
            }
            cmd[n] += base;
        }
        commands.insert(commands.end(), cmd, cmd + G545_LIVE_WORDS);
        packed.insert(packed.end(), blob.begin() + 128, blob.end());
    }
    while ((packed.size() & 3u) != 0u)
        packed.push_back(0u);
    payload.resize(packed.size() / 4u);
    for (size_t n = 0u; n < payload.size(); ++n)
        payload[n] = g545ReadU32(packed.data() + n * 4u);
    return true;
}

static bool g545LinkProgram(GLFns &f, GLuint &program, GLint &commandCountLoc,
                            std::string &why)
{
    const std::string source = g545LiveShaderSource(why);
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
    commandCountLoc = f.glGetUniformLocation_(program, "uG545CommandCount");
    if (commandCountLoc < 0)
    {
        why = "missing uG545CommandCount uniform";
        return false;
    }
    return true;
}

static bool g545UploadImage(GLuint &texture, uint32_t &capacityRows,
                            const std::vector<uint32_t> &words, std::string &why)
{
    uint32_t rows = static_cast<uint32_t>((words.size() + G542_IMAGE_WIDTH - 1u) /
                                          G542_IMAGE_WIDTH);
    if (rows == 0u)
        rows = 1u;
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (maxSize < static_cast<GLint>(G542_IMAGE_WIDTH) || rows > static_cast<uint32_t>(maxSize))
    {
        why = "upload image exceeds GL_MAX_TEXTURE_SIZE";
        return false;
    }
    if (texture == 0u)
    {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    else
        glBindTexture(GL_TEXTURE_2D, texture);

    if (rows > capacityRows)
    {
        uint32_t grown = 1u;
        while (grown < rows)
            grown <<= 1u;
        if (grown > static_cast<uint32_t>(maxSize))
            grown = rows;
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL541_R32UI),
                     static_cast<GLsizei>(G542_IMAGE_WIDTH), static_cast<GLsizei>(grown), 0,
                     GL541_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        capacityRows = grown;
    }
    std::vector<uint32_t> padded(static_cast<size_t>(rows) * G542_IMAGE_WIDTH, 0u);
    std::copy(words.begin(), words.end(), padded.begin());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(G542_IMAGE_WIDTH),
                    static_cast<GLsizei>(rows), GL541_RED_INTEGER, GL_UNSIGNED_INT,
                    padded.data());
    const GLenum err = glGetError();
    if (texture == 0u || err != GL_NO_ERROR)
    {
        why = "R32UI upload failed gl=" + std::to_string(static_cast<unsigned>(err));
        return false;
    }
    return true;
}
