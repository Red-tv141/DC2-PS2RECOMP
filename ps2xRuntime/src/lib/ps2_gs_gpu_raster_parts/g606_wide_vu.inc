// G606 - the WIDE offline VU1 executor: throughput bench + kick-independence census.
//
// ============================ WHY THIS FILE EXISTS ============================
// Every GPU-VU number this project has ever quoted was produced by a ONE-LANE shader.
//
//   * G542/G543/G544 declare `layout(local_size_x=1, local_size_y=1, local_size_z=1)` and
//     dispatch `glDispatchCompute(records,1,1)`. On NVIDIA a 1-thread workgroup still occupies a
//     whole 32-lane warp, so 31/32 of every warp is idle by construction.
//   * G545/G566 - the LIVE authority path - go further: their main() opens with
//     `if (gl_GlobalInvocationID.x != 0u) return;` and then walks the whole batch in a serial
//     `for (uint rec=0u; rec<commandCount; ++rec)` loop. That is ONE lane of 1280 on the host GPU,
//     with every VU register/RAM access an unhidden ~400-cycle image round trip because there is
//     no second thread to hide it behind.
//
// G566's recorded verdict - "exact, but 16-kick batches take 32-340 ms and COMPUTE dominates" -
// is therefore a 1/1280-wide measurement. 16 kicks x ~1.3 k pairs = ~21 k pair-steps in 32 ms is
// ~1.5 us per pair-step, which is exactly what one GPU lane costs when every operand access is a
// dependent global-memory round trip. It is a property of that implementation, not of GPU VU1
// execution. The same file also carries `uvec4 Rram[1024]` + `uvec4 Rcode[1024]` as PRIVATE
// arrays - 32 KiB of per-invocation local memory, the spill class that was already recorded as
// "99% GPU load, window unresponsive, CPU idle".
//
// This gate re-runs the SAME proven-exact G543 semantic source over a real captured corpus with a
// configurable workgroup width, times the dispatch, and optionally publishes per-record VU
// data-RAM read/write bitmaps so the "can W kicks run concurrently" question can be answered
// offline, with no game route and no timing noise. It is default-off, offline, and never feeds a
// guest byte: the CPU interpreter stays authoritative in every arm.
//
// FLAGS
//   DC2_G606_VUBENCH=1        run it (default off)
//   DC2_G606_LOCAL=<n>        workgroup width, default 64, clamp 1..1024. `1` reproduces G543.
//   DC2_G606_ITERS=<n>        timed repeats, default 8, clamp 1..64
//   DC2_G606_TRACE=1          also publish per-record RAM read/write bitmaps + independence census
//   DC2_G606_WINDOW=<n>       independence window (consecutive kicks per batch), default 16
//   DC2_G606_CORPUS_PATH=...  falls back to DC2_G543_CORPUS_PATH, DC2_G542_CORPUS_PATH, default

constexpr uint32_t G606_TRACE_WORDS_PER_RECORD = 64u; // 32 read + 32 write bitmap words

static uint32_t g606PopCount(uint32_t v)
{
    v = v - ((v >> 1u) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u);
    v = (v + (v >> 4u)) & 0x0f0f0f0fu;
    return (v * 0x01010101u) >> 24u;
}

static uint32_t g606EnvU32(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0')
        return fallback;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 0);
    if (end == v)
        return fallback;
    return static_cast<uint32_t>(std::max<unsigned long>(lo, std::min<unsigned long>(hi, parsed)));
}

static bool g606TraceRequested()
{
    static const bool on = g158EnvFlag("DC2_G606_TRACE");
    return on;
}

// Build the wide variant from G543's exact source. Only the workgroup declaration, a bounds
// guard, and (under DC2_G606_TRACE) the four RAM accessors change; every opcode, pipeline and
// float helper stays token-identical, so the executor's ARITHMETIC is G543's by construction.
// lockstep >= 0 selects ONE record for EVERY invocation. That removes control-flow DIVERGENCE
// between the lanes of a warp (all lanes sit on the same VU PC executing the same opcode) while
// keeping every other property of the executor identical. It is a diagnostic, not a mode: it
// answers "is this interpreter's cost SIMD divergence, or is it memory?" and nothing else.
static std::string g606WideShaderSource(uint32_t records, uint32_t local, uint32_t repeat,
                                        int lockstep, bool trace, std::string &why)
{
    std::string source(g543SemanticShaderSource());

    char decl[128];
    std::snprintf(decl, sizeof(decl),
                  "layout(local_size_x=%u, local_size_y=1, local_size_z=1) in;", local);
    if (!g544ReplaceShaderPart(source,
                               "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;",
                               decl, why))
        return {};

    // OCCUPANCY PROBE (DC2_G606_REPEAT>1). The corpus caps the number of concurrent kicks at its
    // record count; a GPU wants far more threads in flight than that to hide the interpreter's
    // dependent-access latency. Folding the invocation id back onto the record set runs the same
    // work N times over in ONE dispatch, so throughput-vs-threads can be separated from
    // throughput-vs-work. Duplicate invocations write IDENTICAL values to the same output words
    // (same input, deterministic shader), so the exactness comparison below stays valid.
    char guard[256];
    if (lockstep >= 0)
        std::snprintf(guard, sizeof(guard),
                      "    uint rec = gl_GlobalInvocationID.x;\n"
                      "    if (rec >= %uu) return;\n"
                      "    rec = %uu;\n",
                      records * repeat, (uint32_t)lockstep);
    else
        std::snprintf(guard, sizeof(guard),
                      "    uint rec = gl_GlobalInvocationID.x;\n"
                      "    if (rec >= %uu) return;\n"
                      "    rec = rec %% %uu;\n",
                      records * repeat, records);
    if (!g544ReplaceShaderPart(source, "    uint rec = gl_GlobalInvocationID.x;\n", guard, why))
        return {};

    if (!trace)
        return source;

    // Per-invocation 1024-bit read/write bitmaps over VU1 data RAM, flushed once at the end of
    // main() into a private trace image. Armed only AFTER loadRecord(), so the bulk pre-image
    // copy is not counted as a write.
    const char *traceGlobals =
        "layout(r32ui, binding=5) writeonly uniform uimage2D gG606Trace;\n"
        "uint g606Rd[32];\n"
        "uint g606Wr[32];\n"
        "bool g606Armed;\n"
        "uint g606Rec;\n"
        // LIVE-IN, not "read". A quadword this kick wrote before reading carries nothing from an
        // earlier kick, so it is not a dependency. Only a read that is NOT preceded by this kick's
        // own write to the same quadword can observe carried state.
        "void g606MarkRead(uint q) {\n"
        "    uint w = (q >> 5u) & 31u; uint b = 1u << (q & 31u);\n"
        "    if (g606Armed && (g606Wr[w] & b) == 0u) g606Rd[w] |= b;\n"
        "}\n"
        "void g606MarkWrite(uint q) { if (g606Armed) g606Wr[(q >> 5u) & 31u] |= 1u << (q & 31u); }\n"
        "void g606Flush() {\n"
        "    uint base = g606Rec * 64u;\n"
        "    for (uint w = 0u; w < 32u; ++w) {\n"
        "        imageStore(gG606Trace, lc(base + w), uvec4(g606Rd[w], 0u, 0u, 0u));\n"
        "        imageStore(gG606Trace, lc(base + 32u + w), uvec4(g606Wr[w], 0u, 0u, 0u));\n"
        "    }\n"
        "}\n"
        "uvec4 g543LoadRamQuad(uint q);\n";
    if (!g544ReplaceShaderPart(source, "uvec4 g543LoadRamQuad(uint q)\n",
                               std::string(traceGlobals) + "uvec4 g543LoadRamQuad(uint q)\n", why))
        return {};

    if (!g544ReplaceShaderPart(source, "    return Rram[q];\n",
                               "    g606MarkRead(q); return Rram[q];\n", why) ||
        !g544ReplaceShaderPart(source, "    Rram[q] = value;\n",
                               "    g606MarkWrite(q); Rram[q] = value;\n", why) ||
        !g544ReplaceShaderPart(source, "    return Rram[q][lane];\n",
                               "    g606MarkRead(q); return Rram[q][lane];\n", why) ||
        !g544ReplaceShaderPart(source, "    Rram[q][lane] = value;\n",
                               "    g606MarkWrite(q); Rram[q][lane] = value;\n", why))
        return {};

    if (!g544ReplaceShaderPart(source, "    loadRecord();\n    armHistory();\n",
                               "    g606Rec = rec; g606Armed = false;\n"
                               "    for (uint z = 0u; z < 32u; ++z) { g606Rd[z] = 0u; g606Wr[z] = 0u; }\n"
                               "    loadRecord();\n    armHistory();\n"
                               "    g606Armed = true;\n", why))
        return {};

    // storeRecord() reads EVERY quadword of the RAM image to publish postData. Left armed it
    // saturates the read set at 1024/1024 and the census reports a conflict on every member -- a
    // measurement of the instrument, not of the microprogram. Disarm before the publication pass.
    if (!g544ReplaceShaderPart(source, "    storeRecord();\n",
                               "    g606Armed = false;\n    storeRecord();\n", why))
        return {};

    if (!g544ReplaceShaderPart(source, "    sw(statusAt + 7u, gPacketBytes);\n",
                               "    sw(statusAt + 7u, gPacketBytes);\n    g606Flush();\n", why))
        return {};

    return source;
}

static bool g606LinkProgram(GLFns &f, const std::string &source, GLuint &program, std::string &why)
{
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

// Offline independence census. A batch of `window` consecutive kicks can be executed CONCURRENTLY
// from one shared base image if no member reads a quadword an earlier member's EXECUTION wrote and
// that no later VIF write re-supplied. Read/write sets come from the shader; the VIF write extents
// come from the corpus command stream, which is the same data the live path already stages.
static void g606IndependenceCensus(const G542AbiCorpus &corpus,
                                   const std::vector<uint32_t> &trace,
                                   const std::vector<std::vector<uint32_t>> &vifQw,
                                   uint32_t window)
{
    const uint32_t n = corpus.records;
    if (n == 0u || window == 0u)
        return;
    uint64_t members = 0ull, conflicts = 0ull, batches = 0ull, cleanBatches = 0ull;
    uint32_t worstConflictQw = 0u;
    uint64_t readQwTotal = 0ull, writeQwTotal = 0ull;
    uint32_t conflictMap[32] = {0u};
    for (uint32_t base = 0u; base + window <= n; base += window)
    {
        uint32_t carried[32] = {0u};
        bool batchClean = true;
        ++batches;
        for (uint32_t k = 0u; k < window; ++k)
        {
            const uint32_t j = base + k;
            const size_t t = static_cast<size_t>(j) * G606_TRACE_WORDS_PER_RECORD;
            if (t + G606_TRACE_WORDS_PER_RECORD > trace.size())
                return;
            // A VIF write to a quadword re-supplies it, so an earlier exec write to it is no
            // longer observable by this kick or any later one.
            uint32_t supplied[32] = {0u};
            for (uint32_t q : vifQw[j])
                supplied[(q >> 5u) & 31u] |= 1u << (q & 31u);
            for (uint32_t w = 0u; w < 32u; ++w)
                carried[w] &= ~supplied[w];

            uint32_t hit = 0u;
            for (uint32_t w = 0u; w < 32u; ++w)
            {
                const uint32_t rd = trace[t + w];
                const uint32_t wr = trace[t + 32u + w];
                readQwTotal += static_cast<uint64_t>(g606PopCount(rd));
                writeQwTotal += static_cast<uint64_t>(g606PopCount(wr));
                hit += g606PopCount(rd & carried[w]);
                conflictMap[w] |= rd & carried[w];
            }
            ++members;
            if (hit != 0u)
            {
                ++conflicts;
                batchClean = false;
                worstConflictQw = std::max(worstConflictQw, hit);
            }
            for (uint32_t w = 0u; w < 32u; ++w)
                carried[w] |= trace[t + 32u + w];
        }
        if (batchClean)
            ++cleanBatches;
    }
    std::fprintf(stderr,
                 "[G606:indep] window=%u batches=%llu cleanBatches=%llu (%.1f%%) members=%llu "
                 "conflictMembers=%llu (%.2f%%) worstConflictQw=%u | readQw/kick=%.1f "
                 "writeQw/kick=%.1f (of 1024)\n",
                 window, (unsigned long long)batches, (unsigned long long)cleanBatches,
                 100.0 * (double)cleanBatches / (double)(batches ? batches : 1ull),
                 (unsigned long long)members, (unsigned long long)conflicts,
                 100.0 * (double)conflicts / (double)(members ? members : 1ull),
                 worstConflictQw,
                 (double)readQwTotal / (double)(members ? members : 1ull),
                 (double)writeQwTotal / (double)(members ? members : 1ull));

    // WHERE the carried dependency lives. If it is a small fixed region the batch can be split by
    // ADDRESS instead of being abandoned; if it is spread over the map it cannot.
    uint32_t distinct = 0u;
    for (uint32_t w = 0u; w < 32u; ++w)
        distinct += g606PopCount(conflictMap[w]);
    char list[512];
    int at = 0;
    uint32_t shown = 0u;
    for (uint32_t q = 0u; q < 1024u && shown < 24u; ++q)
        if ((conflictMap[(q >> 5u) & 31u] & (1u << (q & 31u))) != 0u)
        {
            at += std::snprintf(list + at, sizeof(list) - (size_t)at, " %u", q);
            ++shown;
            if (at < 0 || (size_t)at >= sizeof(list) - 8u)
                break;
        }
    std::fprintf(stderr, "[G606:conflictmap] window=%u distinctQw=%u of 1024 | first:%s\n",
                 window, distinct, at > 0 ? list : " none");
}

static bool g606RunWideBench(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr)
    {
        std::fprintf(stderr, "[G606:bench] FAIL compute entry points unavailable; CPU path unchanged\n");
        return false;
    }

    const char *envPath = std::getenv("DC2_G606_CORPUS_PATH");
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
        std::fprintf(stderr, "[G606:bench] FAIL path='%s': %s; CPU path unchanged\n",
                     path.string().c_str(), why.c_str());
        return false;
    }

    for (uint32_t r = 0u; r < corpus.records; ++r)
        corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 31u] =
            static_cast<uint32_t>(corpus.payload.size() +
                                  static_cast<size_t>(r) * G543_STATUS_WORDS);

    const bool trace = g606TraceRequested();
    const uint32_t local = g606EnvU32("DC2_G606_LOCAL", 64u, 1u, 1024u);
    const uint32_t iters = g606EnvU32("DC2_G606_ITERS", 8u, 1u, 64u);
    const uint32_t window = g606EnvU32("DC2_G606_WINDOW", 16u, 1u, 1024u);
    const uint32_t repeat = g606EnvU32("DC2_G606_REPEAT", 1u, 1u, 64u);
    const uint32_t lockstepPlus1 = g606EnvU32("DC2_G606_LOCKSTEP", 0u, 0u, 4096u);
    const int lockstep = (lockstepPlus1 == 0u || lockstepPlus1 > corpus.records)
        ? -1 : (int)(lockstepPlus1 - 1u);

    const std::string source =
        g606WideShaderSource(corpus.records, local, repeat, lockstep, trace, why);
    if (source.empty())
    {
        std::fprintf(stderr, "[G606:bench] FAIL shader build: %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    G542GlObjects gl;
    gl.f = &f;
    if (!g606LinkProgram(f, source, gl.program, why))
    {
        std::fprintf(stderr, "[G606:bench] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {}
    uint32_t commandHeight = 0u, payloadHeight = 0u, outputHeight = 0u, traceHeight = 0u;
    std::vector<uint32_t> outputInit(
        corpus.payload.size() + static_cast<size_t>(corpus.records) * G543_STATUS_WORDS,
        0xa5a5a5a5u);
    GLuint traceTex = 0u;
    std::vector<uint32_t> traceInit;
    if (trace)
        traceInit.assign(static_cast<size_t>(corpus.records) * G606_TRACE_WORDS_PER_RECORD, 0u);
    if (!g542CreateImage(gl.textures[0], corpus.commands, commandHeight, why) ||
        !g542CreateImage(gl.textures[1], corpus.payload, payloadHeight, why) ||
        !g542CreateImage(gl.textures[2], outputInit, outputHeight, why) ||
        (trace && !g542CreateImage(traceTex, traceInit, traceHeight, why)))
    {
        std::fprintf(stderr, "[G606:bench] FAIL %s; CPU path unchanged\n", why.c_str());
        if (traceTex != 0u)
            glDeleteTextures(1, &traceTex);
        return false;
    }

    f.glUseProgram_(gl.program);
    e.glBindImageTexture_(2u, gl.textures[0], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(3u, gl.textures[1], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(4u, gl.textures[2], 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
    if (trace)
        e.glBindImageTexture_(5u, traceTex, 0, GL_FALSE, 0, GL542_WRITE_ONLY, GL541_R32UI);

    const uint32_t groups = (corpus.records * repeat + local - 1u) / local;

    // Warm-up dispatch: shader/driver first-use cost is not the executor's throughput.
    e.glDispatchCompute_(groups, 1u, 1u);
    e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL178_TEXTURE_UPDATE_BARRIER_BIT);
    glFinish();

    double best = 1e30, sum = 0.0;
    for (uint32_t i = 0u; i < iters; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        e.glDispatchCompute_(groups, 1u, 1u);
        e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL178_TEXTURE_UPDATE_BARRIER_BIT);
        glFinish();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        sum += ms;
    }

    std::vector<uint32_t> output(static_cast<size_t>(G542_IMAGE_WIDTH) * outputHeight);
    glBindTexture(GL_TEXTURE_2D, gl.textures[2]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, output.data());
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        std::fprintf(stderr, "[G606:bench] FAIL readback gl=0x%x; CPU path unchanged\n",
                     static_cast<unsigned>(err));
        if (traceTex != 0u)
            glDeleteTextures(1, &traceTex);
        return false;
    }

    // Exactness: identical section comparison to G543's promotion-grade oracle.
    uint32_t bad = 0u, checked = 0u;
    uint64_t pairs = 0ull, packetBytes = 0ull;
    G543Mismatch firstMismatch;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        // Under LOCKSTEP only the selected record was executed; every other output slot still
        // holds its 0xa5a5a5a5 fill, so comparing them would report a mismatch that means nothing.
        if (lockstep >= 0 && r != static_cast<uint32_t>(lockstep))
            continue;
        const size_t c = static_cast<size_t>(r) * G542_ABI_WORDS;
        const uint32_t *cmd = corpus.commands.data() + c;
        const size_t s = corpus.commands[c + 31u];
        pairs += cmd[10];
        packetBytes += cmd[29];
        ++checked;
        if (s + G543_STATUS_WORDS > output.size() || output[s] != G543_STATUS_MAGIC ||
            output[s + 1u] != 0u)
        {
            if (bad++ == 0u)
                std::fprintf(stderr,
                             "[G606:bench] STATUS record=%u magic=%08x flags=%08x pc=%04x cycles=%u/%u\n",
                             r, s < output.size() ? output[s] : 0u,
                             s + 1u < output.size() ? output[s + 1u] : 0xffffffffu,
                             s + 2u < output.size() ? output[s + 2u] : 0xffffffffu,
                             s + 4u < output.size() ? output[s + 4u] : 0u, cmd[10]);
            continue;
        }
        G543Mismatch mismatch;
        if (!g543CompareSection(corpus, output, r, "postState", cmd[16], 632u, mismatch) ||
            !g543CompareSection(corpus, output, r, "postPipe", cmd[18], 644u, mismatch) ||
            !g543CompareSection(corpus, output, r, "postData", cmd[21], 16384u, mismatch) ||
            !g543CompareSection(corpus, output, r, "packetDesc", cmd[26], cmd[27] * 8u, mismatch) ||
            !g543CompareSection(corpus, output, r, "packetBytes", cmd[28], cmd[29], mismatch))
        {
            if (bad++ == 0u)
                firstMismatch = mismatch;
        }
    }

    const double meanMs = sum / (double)iters;
    const uint32_t threads = corpus.records * repeat;
    const uint64_t execPairs = lockstep >= 0 ? pairs * threads : pairs * repeat;
    const double usPerRecord = best * 1000.0 / (double)(threads ? threads : 1u);
    const double pairsPerSec = best > 0.0 ? (double)execPairs / (best / 1000.0) : 0.0;
    std::fprintf(stderr,
                 "[G606:bench] local=%u repeat=%u lockstep=%d threads=%u groups=%u records=%u "
                 "pairs=%llu iters=%u | bestMs=%.3f meanMs=%.3f | usPerKick=%.3f "
                 "pairsPerSec=%.3fM | bad=%u checked=%u trace=%d\n",
                 local, repeat, lockstep, threads, groups, corpus.records,
                 (unsigned long long)execPairs, iters, best, meanMs,
                 usPerRecord, pairsPerSec / 1.0e6, bad, checked, trace ? 1 : 0);
    if (bad != 0u && firstMismatch.record != 0xffffffffu)
        std::fprintf(stderr,
                     "[G606:bench] MISMATCH record=%u section=%s byte=%u expected=%02x actual=%02x "
                     "expectedWord=%08x actualWord=%08x\n",
                     firstMismatch.record, firstMismatch.section, firstMismatch.byte,
                     (unsigned)firstMismatch.expected, (unsigned)firstMismatch.actual,
                     firstMismatch.expectedWord, firstMismatch.actualWord);

    if (trace && lockstep < 0)
    {
        std::vector<uint32_t> traceOut(static_cast<size_t>(G542_IMAGE_WIDTH) * traceHeight);
        glBindTexture(GL_TEXTURE_2D, traceTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, traceOut.data());
        if (glGetError() == GL_NO_ERROR)
        {
            // VIF write extents per record, in quadwords, straight from the command stream.
            std::vector<std::vector<uint32_t>> vifQw(corpus.records);
            const auto *payloadBytes = reinterpret_cast<const uint8_t *>(corpus.payload.data());
            for (uint32_t r = 0u; r < corpus.records; ++r)
            {
                const uint32_t *cmd = corpus.commands.data() +
                                      static_cast<size_t>(r) * G542_ABI_WORDS;
                const uint32_t writeDescByte = cmd[22];
                const uint32_t writeCount = cmd[23];
                for (uint32_t w = 0u; w < writeCount; ++w)
                {
                    const uint64_t at = static_cast<uint64_t>(writeDescByte) + w * 12ull;
                    if (at + 12ull > (uint64_t)corpus.payload.size() * 4ull)
                        break;
                    const uint32_t off = g542ReadU32(payloadBytes + at);
                    const uint32_t len = g542ReadU32(payloadBytes + at + 4u);
                    const uint32_t isCode = g542ReadU32(payloadBytes + at + 8u);
                    if (isCode != 0u || len == 0u)
                        continue;
                    for (uint32_t q = off / 16u; q < (off + len + 15u) / 16u; ++q)
                        vifQw[r].push_back(q & 1023u);
                }
            }
            const uint32_t sweep[] = {2u, 4u, 8u, 16u, 32u, 64u, 128u};
            for (uint32_t w : sweep)
                g606IndependenceCensus(corpus, traceOut, vifQw, w);
            if (window != 2u && window != 4u && window != 8u && window != 16u &&
                window != 32u && window != 64u && window != 128u)
                g606IndependenceCensus(corpus, traceOut, vifQw, window);
        }
        glDeleteTextures(1, &traceTex);
    }

    return bad == 0u;
}
