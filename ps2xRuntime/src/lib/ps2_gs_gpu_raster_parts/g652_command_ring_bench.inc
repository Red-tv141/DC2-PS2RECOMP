// G652 P17: finite persistent-mapped command-ring microprototype.
//
// This is deliberately an offline mechanism proof, not a VU1 authority path. One persistently
// mapped SSBO receives N commands, one compute dispatch consumes them in order, and every command
// reads the state produced by its predecessor. There is no CPU round trip or dispatch between
// commands. The finite loop avoids a Windows TDR-risking resident spin kernel. Integer arithmetic
// gives the prototype a bit-exact CPU oracle; GLSL-vs-SSE VU floating-point exactness remains a
// separate promotion blocker.
//
//   DC2_G652_RINGBENCH=1
//   DC2_G652_RING_COMMANDS=<2..4096> (default 128)
//   DC2_G652_RING_WORK=<1..1024>     (default 64 dependent steps/command)
//   DC2_G652_RING_ITERS=<1..64>      (default 12 timed dispatches)

static bool g652RingBenchRequested()
{
    static const bool on = g158EnvFlag("DC2_G652_RINGBENCH");
    return on;
}

static uint32_t g652RingEnv(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return fallback;
    char *end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 0);
    return end == v ? fallback
                    : static_cast<uint32_t>(std::max<unsigned long>(lo,
                                                std::min<unsigned long>(hi, n)));
}

static uint32_t g652RingStep(uint32_t state, uint32_t a, uint32_t b)
{
    state ^= a + 0x9E3779B9u;
    state = (state << 5u) | (state >> 27u);
    return state * 1664525u + b + 1013904223u;
}

static bool g652RunRingBench(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr || e.glBindBufferBase_ == nullptr ||
        e.glBufferStorage_ == nullptr || e.glMapBufferRange_ == nullptr ||
        e.glUnmapBuffer_ == nullptr || f.glGenBuffers_ == nullptr ||
        f.glBindBuffer_ == nullptr || f.glDeleteBuffers_ == nullptr)
    {
        std::fprintf(stderr,
                     "[G652:ring] BLOCKED persistent-map entry points unavailable; CPU path unchanged\n");
        return false;
    }

    const uint32_t commands = g652RingEnv("DC2_G652_RING_COMMANDS", 128u, 2u, 4096u);
    const uint32_t work = g652RingEnv("DC2_G652_RING_WORK", 64u, 1u, 1024u);
    const uint32_t iters = g652RingEnv("DC2_G652_RING_ITERS", 12u, 1u, 64u);
    std::vector<uint32_t> commandWords(static_cast<size_t>(commands) * 4u);
    for (uint32_t i = 0u; i < commands; ++i)
    {
        commandWords[i * 4u + 0u] = i * 2246822519u + 1u;
        commandWords[i * 4u + 1u] = i * 3266489917u + 3u;
        commandWords[i * 4u + 2u] = i;
        commandWords[i * 4u + 3u] = 0xA5A5A5A5u ^ i;
    }

    char defines[160];
    std::snprintf(defines, sizeof(defines),
                  "#version 430 core\n#define COMMANDS %uu\n#define WORK %uu\n",
                  commands, work);
    std::string source(defines);
    source += R"GLSL(
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(std430, binding=6) readonly buffer Ring { uvec4 commands[]; };
layout(r32ui, binding=4) coherent uniform uimage2D outputWord;
void main()
{
    uint state = 0x12345678u;
    for (uint i = 0u; i < COMMANDS; ++i)
    {
        uvec4 c = commands[i];
        for (uint w = 0u; w < WORK; ++w)
        {
            state ^= c.x + 0x9E3779B9u;
            state = (state << 5u) | (state >> 27u);
            state = state * 1664525u + c.y + 1013904223u;
        }
        state ^= c.z + c.w;
    }
    imageStore(outputWord, ivec2(0, 0), uvec4(state, 0u, 0u, 0u));
}
)GLSL";

    std::string why;
    GLuint program = 0u;
    if (!g606LinkProgram(f, source, program, why))
    {
        std::fprintf(stderr, "[G652:ring] BLOCKED shader: %s\n", why.c_str());
        return false;
    }

    GLuint ring = 0u;
    f.glGenBuffers_(1, &ring);
    f.glBindBuffer_(GL643_SHADER_STORAGE_BUFFER, ring);
    constexpr GLbitfield kMapWrite = 0x0002u;
    constexpr GLbitfield kMapPersistent = 0x0040u;
    constexpr GLbitfield kMapCoherent = 0x0080u;
    constexpr GLbitfield kDynamicStorage = 0x0100u;
    const GLsizeiptrLocal ringBytes =
        static_cast<GLsizeiptrLocal>(commandWords.size() * sizeof(uint32_t));
    e.glBufferStorage_(GL643_SHADER_STORAGE_BUFFER, ringBytes, nullptr,
                       kMapWrite | kMapPersistent | kMapCoherent | kDynamicStorage);
    void *mapped = e.glMapBufferRange_(GL643_SHADER_STORAGE_BUFFER, 0, ringBytes,
                                      kMapWrite | kMapPersistent | kMapCoherent);
    if (ring == 0u || mapped == nullptr || glGetError() != GL_NO_ERROR)
    {
        std::fprintf(stderr, "[G652:ring] BLOCKED persistent mapping refused\n");
        if (ring != 0u)
            f.glDeleteBuffers_(1, &ring);
        f.glDeleteProgram_(program);
        return false;
    }
    std::memcpy(mapped, commandWords.data(), static_cast<size_t>(ringBytes));

    std::vector<uint32_t> init(1u, 0u);
    GLuint outTex = 0u;
    uint32_t outHeight = 0u;
    if (!g542CreateImage(outTex, init, outHeight, why))
    {
        e.glUnmapBuffer_(GL643_SHADER_STORAGE_BUFFER);
        f.glDeleteBuffers_(1, &ring);
        f.glDeleteProgram_(program);
        std::fprintf(stderr, "[G652:ring] BLOCKED output image: %s\n", why.c_str());
        return false;
    }

    f.glUseProgram_(program);
    e.glBindBufferBase_(GL643_SHADER_STORAGE_BUFFER, 6u, ring);
    e.glBindImageTexture_(4u, outTex, 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
    e.glDispatchCompute_(1u, 1u, 1u);
    e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL643_SHADER_STORAGE_BARRIER_BIT);
    glFinish();

    double bestMs = 1e30;
    double sumMs = 0.0;
    for (uint32_t i = 0u; i < iters; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        e.glDispatchCompute_(1u, 1u, 1u);
        e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                           GL643_SHADER_STORAGE_BARRIER_BIT);
        glFinish();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        bestMs = std::min(bestMs, ms);
        sumMs += ms;
    }

    uint32_t expected = 0x12345678u;
    for (uint32_t i = 0u; i < commands; ++i)
    {
        const uint32_t a = commandWords[i * 4u + 0u];
        const uint32_t b = commandWords[i * 4u + 1u];
        for (uint32_t w = 0u; w < work; ++w)
            expected = g652RingStep(expected, a, b);
        expected ^= commandWords[i * 4u + 2u] + commandWords[i * 4u + 3u];
    }
    std::vector<uint32_t> readback(static_cast<size_t>(G542_IMAGE_WIDTH) * outHeight);
    glBindTexture(GL_TEXTURE_2D, outTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT,
                  readback.data());
    const uint32_t got = readback[0];
    const GLenum err = glGetError();
    std::fprintf(stderr,
                 "[G652:ring] commands=%u work=%u mapped=1 hostWrites=1 dispatches=1 "
                 "midRoundTrips=0 best=%.3fms mean=%.3fms us/command=%.3f "
                 "oracle=%s got=0x%08x expected=0x%08x gl=0x%x\n",
                 commands, work, bestMs, sumMs / static_cast<double>(iters),
                 bestMs * 1000.0 / static_cast<double>(commands),
                 (got == expected && err == GL_NO_ERROR) ? "PASS" : "FAIL",
                 got, expected, static_cast<unsigned>(err));

    f.glUseProgram_(0u);
    f.glBindBuffer_(GL643_SHADER_STORAGE_BUFFER, ring);
    (void)e.glUnmapBuffer_(GL643_SHADER_STORAGE_BUFFER);
    f.glBindBuffer_(GL643_SHADER_STORAGE_BUFFER, 0u);
    f.glDeleteBuffers_(1, &ring);
    glDeleteTextures(1, &outTex);
    f.glDeleteProgram_(program);
    return got == expected && err == GL_NO_ERROR;
}
