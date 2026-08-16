// =====================================================================================
// G607 — TRANSLATED VU1 kernel throughput CEILING bench (default OFF, DC2_G607_TBENCH=1)
//
// WHAT QUESTION THIS ANSWERS
// --------------------------
// G606 closed the interpreter-shaped GPU VU1 executor and pointed at TRANSLATION as the only
// remaining shape: stop dispatching opcodes, compile the microprogram, and map a lane to a DATA
// ITEM inside one program.  It also listed the two things that had to be priced before any of it
// was built.  `tools/g607_micro.py` answered the first from the static CFG plus a 1024-kick live
// control-flow profile of `ridepod`:
//
//   * the static walk covers 100.0000% of 1,187,573 executed pairs -- translation has complete
//     static knowledge of this microprogram;
//   * the hot per-item loops are BRANCH-FREE, so the divergence mechanism G606 priced at 10.3x
//     does NOT reappear inside a translated kernel; and
//   * the width a translated kernel can launch is the loop's per-entry trip count, whose
//     PAIR-WEIGHTED distribution is p50 = 5, mean = 10.7, max 57 -- 28.4% occupancy on a 32-lane
//     warp, 0.00% of loop-body pairs at width >= 64.
//
// That leaves exactly one unmeasured term: how much cheaper a TRANSLATED pair-step is than
// G606's INTERPRETED one (16.5 ns of GPU slot time; 59.9 M pair-steps/s saturated and uniform;
// 73 M for one CPU core on the same route).  This bench measures it directly, at the widths the
// profile says are actually available.
//
// WHY IT IS A CEILING, DELIBERATELY
// ---------------------------------
// The emitted body (tools/g607_emit.py -> g607_translated_body.inc) models the FMAC/FDIV/EFU
// arithmetic and the VU data-RAM traffic, and NOTHING else.  It does not model the Q/P pipeline
// latencies, the MAC/status/clip flag writes, the hidden scalar pipeline, or XGKICK packet
// emission -- all of which are real work a promotable backend must do.  Every omission makes the
// GPU look FASTER.  It also runs in the `native` arm on raw GLSL float, which is not VU-exact.
//
// So this is an UPPER BOUND on translated GPU VU1 throughput, and it is built that way on
// purpose: a ceiling that already fails closes the architecture without anyone having to build
// the exact version first (the project's price-before-you-build law).  A ceiling that PASSES is
// not a promotion -- it is permission to go build the exact one.
//
// The `exact` arm additionally routes RSQRT through G543's proven-exact double-precision helper.
// On this host (GTX 1060, Pascal) FP64 runs at 1/32 rate, so the native/exact gap is itself a
// result: it is the price of VU-exactness on this GPU, which no amount of translation removes.
//
// It is offline: it runs once at GL-worker startup, touches no guest byte, and the CPU VU1
// interpreter stays authoritative in every arm.
//
// FLAGS
//   DC2_G607_TBENCH=1        run it
//   DC2_G607_TWIDTH=<n>      lanes per workgroup = loop iterations per kick (default 32, 1..1024)
//   DC2_G607_TGROUPS=<n>     workgroups = kicks in flight (default 512, 1..65535)
//   DC2_G607_TREPEAT=<n>     body repeats per lane (default 8) -- amortises launch so the number
//                            quoted is THROUGHPUT, not dispatch overhead
//   DC2_G607_TITERS=<n>      timed dispatches, best-of (default 8, 1..64)
//   DC2_G607_TEXACT=1        use the exact FP64 RSQRT arm instead of native
// =====================================================================================

static uint32_t g607EnvU32(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
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

static bool g607TBenchRequested()
{
    static const bool on = g158EnvFlag("DC2_G607_TBENCH");
    return on;
}

// The scaffolding around the generated body. `shared uvec4 sram[1024]` is the modelling decision
// that matters: one WORKGROUP is one KICK and its lanes are that kick's loop iterations, so the
// kick's 16 KiB of VU1 data RAM is shared by the lanes exactly as it is on the real VU. Each lane
// gets its own streaming pointers, so lanes read and write different quadwords -- a gather, which
// is what the real loop does through LQI/SQI.
static std::string g607TBenchShaderSource(uint32_t width, uint32_t repeat, bool exact,
                                          uint32_t fillQw)
{
    std::string s;
    s += "#version 430 core\n";
    s += "#pragma STDGL invariant(all)\n";
    char decl[128];
    std::snprintf(decl, sizeof(decl), "layout(local_size_x=%u, local_size_y=1, local_size_z=1) in;\n",
                  width);
    s += decl;
    s += "layout(r32ui, binding=4) coherent uniform uimage2D gOutput;\n";
    s += "const uint IMAGE_W = 4096u;\n";
    s += "ivec2 lc(uint i) { return ivec2(int(i & 4095u), int(i >> 12u)); }\n";
    s += "void sw(uint i, uint v) { imageStore(gOutput, lc(i), uvec4(v,0u,0u,0u)); }\n";
    s += "shared uvec4 sram[1024];\n";
    s += "const int VI0 = 0;\n";
    s += "const int XTOPV = 0;\n";
    s += "vec4 ramLoad(int q) { return uintBitsToFloat(sram[uint(q) & 1023u]); }\n";
    s += "void ramStore(int q, vec4 v, uint mask) {\n"
         "    uint i = uint(q) & 1023u;\n"
         "    uvec4 cur = sram[i];\n"
         "    uvec4 nw = floatBitsToUint(v);\n"
         "    if ((mask & 8u) != 0u) cur.x = nw.x;\n"
         "    if ((mask & 4u) != 0u) cur.y = nw.y;\n"
         "    if ((mask & 2u) != 0u) cur.z = nw.z;\n"
         "    if ((mask & 1u) != 0u) cur.w = nw.w;\n"
         "    sram[i] = cur;\n"
         "}\n";
    s += "int ramLoadInt(int q, uint mask) {\n"
         "    uvec4 v = sram[uint(q) & 1023u];\n"
         "    uint w = (mask & 8u) != 0u ? v.x : ((mask & 4u) != 0u ? v.y :\n"
         "             ((mask & 2u) != 0u ? v.z : v.w));\n"
         "    return int(w & 0xffffu);\n"
         "}\n";
    s += "void ramStoreInt(int q, int val, uint mask) {\n"
         "    uint i = uint(q) & 1023u;\n"
         "    uvec4 cur = sram[i];\n"
         "    uint w = uint(val) & 0xffffu;\n"
         "    if ((mask & 8u) != 0u) cur.x = w; else if ((mask & 4u) != 0u) cur.y = w;\n"
         "    else if ((mask & 2u) != 0u) cur.z = w; else cur.w = w;\n"
         "    sram[i] = cur;\n"
         "}\n";
    // native arm: whatever the GPU does fastest. exact arm: G543's correctly-rounded path.
    s += "float nativeRsqrt(float a, float b) { return a * inversesqrt(abs(b)); }\n";
    s += "float nativeDiv(float a, float b) { return a / b; }\n";
    s += "precise float exactRsqrt(float a, float b) {\n"
         "    precise double r = sqrt(double(abs(b)));\n"
         "    precise double q = double(a) / r;\n"
         "    return float(q);\n"
         "}\n";
    s += "precise float exactDiv(float a, float b) {\n"
         "    precise double q = double(a) / double(b);\n"
         "    return float(q);\n"
         "}\n";
    s += "precise float exactSqrt(float a) { return float(sqrt(double(a))); }\n";
    s += "float bits(uint h) { return uintBitsToFloat((h & 0x007fffffu) | 0x3f800000u); }\n";
    s += "void main() {\n";
    s += "    uint grp = gl_WorkGroupID.x;\n";
    s += "    uint lane = gl_LocalInvocationID.x;\n";
    // Deterministic, workgroup-dependent RAM fill. Values are finite and in [1,2) so no lane
    // takes a denormal/NaN slow path -- the bench must measure the ordinary case.
    //
    // fillQw is a SEPARATE AXIS on purpose. Materialising the kick's VU data RAM into shared
    // memory is a PER-GROUP cost that does not shrink with the width, so at width 5 it is paid by
    // 5 lanes and at width 256 by 256 -- fold it into the arithmetic number and the width curve
    // reports the sum of two different effects. DC2_G607_TFILL=0 measures the arithmetic and the
    // RAM-access pattern alone; the default 1024 measures it with the whole 16 KiB materialised,
    // which is the pessimistic end (a live path stages only ~785 B/kick of VIF writes, G606).
    char fill[640];
    if (fillQw > 0u)
    {
        std::snprintf(fill, sizeof(fill),
                      "    for (uint i = lane; i < %uu; i += %uu) {\n"
                      "        uint h = (grp * 2654435761u) ^ (i * 2246822519u);\n"
                      "        sram[i] = uvec4(floatBitsToUint(bits(h)), floatBitsToUint(bits(h*3u)),\n"
                      "                        floatBitsToUint(bits(h*5u)), floatBitsToUint(bits(h*7u)));\n"
                      "    }\n"
                      "    barrier();\n"
                      "    memoryBarrierShared();\n",
                      fillQw, width);
    }
    else
    {
        // TFILL=0 still initialises all 1024 quadwords, with a CONSTANT instead of the per-slot
        // hash. Reading uninitialised shared memory would put lanes on denormal/NaN slow paths and
        // silently corrupt the very number this bench exists to produce, so "no fill" means "the
        // cheapest legal fill", not "no writes".
        std::snprintf(fill, sizeof(fill),
                      "    for (uint i = lane; i < 1024u; i += %uu)\n"
                      "        sram[i] = uvec4(0x3fc00000u, 0x3fa00000u, 0x3f900000u, 0x3f800000u);\n"
                      "    barrier();\n"
                      "    memoryBarrierShared();\n",
                      width);
    }
    s += fill;
    s += "    vec4 ACC = vec4(0.0);\n";
    s += "    float Q = 1.0, P = 1.0, I = 1.0;\n";
    s += "    const vec4 VF0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    for (int i = 1; i < 32; ++i)
    {
        char l[160];
        std::snprintf(l, sizeof(l), "    vec4 VF%d = ramLoad(int(lane) * 3 + %d);\n", i, i * 7);
        s += l;
    }
    // Per-lane streaming pointers: lane i works on item i, exactly as a translated kernel would
    // assign one lane per loop iteration.
    for (int i = 1; i < 16; ++i)
    {
        char l[160];
        std::snprintf(l, sizeof(l), "    int VI%d = (int(lane) * 3 + %d) & 1023;\n", i, i * 61);
        s += l;
    }
    char rep[128];
    std::snprintf(rep, sizeof(rep), "    for (uint rep = 0u; rep < %uu; ++rep) {\n", repeat);
    s += rep;
    s += (exact ? g607TranslatedBodyExact() : g607TranslatedBodyNative());
    s += "\n    }\n";
    // Publish something derived from every register class so nothing can be eliminated.
    s += "    vec4 sink = VF17 + VF25 + VF26 + VF31 + ACC + vec4(Q + P);\n";
    s += "    uint o = (grp * gl_WorkGroupSize.x + lane) * 4u;\n";
    s += "    sw(o + 0u, floatBitsToUint(sink.x));\n";
    s += "    sw(o + 1u, floatBitsToUint(sink.y));\n";
    s += "    sw(o + 2u, floatBitsToUint(sink.z));\n";
    s += "    sw(o + 3u, floatBitsToUint(sink.w) ^ uint(VI2 + VI4 + VI10 + VI11));\n";
    s += "}\n";
    return s;
}

static bool g607RunTBench(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr)
    {
        std::fprintf(stderr, "[G607:tbench] FAIL compute entry points unavailable; "
                             "CPU path unchanged\n");
        return false;
    }

    const uint32_t width = g607EnvU32("DC2_G607_TWIDTH", 32u, 1u, 1024u);
    const uint32_t groups = g607EnvU32("DC2_G607_TGROUPS", 512u, 1u, 65535u);
    const uint32_t repeat = g607EnvU32("DC2_G607_TREPEAT", 8u, 1u, 4096u);
    const uint32_t iters = g607EnvU32("DC2_G607_TITERS", 8u, 1u, 64u);
    const bool exact = g158EnvFlag("DC2_G607_TEXACT");
    const uint32_t fillQw = g607EnvU32("DC2_G607_TFILL", 1024u, 0u, 1024u);

    std::string why;
    const std::string source = g607TBenchShaderSource(width, repeat, exact, fillQw);
    GLuint program = 0u;
    if (!g606LinkProgram(f, source, program, why))
    {
        std::fprintf(stderr, "[G607:tbench] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {}
    const size_t outWords = static_cast<size_t>(groups) * width * 4u;
    std::vector<uint32_t> outInit(outWords, 0xa5a5a5a5u);
    GLuint outTex = 0u;
    uint32_t outHeight = 0u;
    if (!g542CreateImage(outTex, outInit, outHeight, why))
    {
        std::fprintf(stderr, "[G607:tbench] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    f.glUseProgram_(program);
    e.glBindImageTexture_(4u, outTex, 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);

    e.glDispatchCompute_(groups, 1u, 1u);   // warm-up: first-use cost is not throughput
    e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL178_TEXTURE_UPDATE_BARRIER_BIT);
    glFinish();

    // CHAIN is the measurement that decides a GPU-RESIDENT design. The cross-kick RAW dependency
    // (G606: 44.1% of kicks read state the immediately preceding kick wrote, 0 clean batches at
    // window >= 4) means consecutive kicks CANNOT be merged into one dispatch -- but they need not
    // each cost a CPU round trip either. Issuing `chain` dispatches back-to-back with only a
    // memory barrier between them, and ONE glFinish at the end, prices the ordering constraint the
    // way a GPU-resident backend would actually pay it: GPU-side serialised dispatch, no CPU sync.
    // chain=1 reproduces the CPU-round-trip floor.
    const uint32_t chain = g607EnvU32("DC2_G607_TCHAIN", 1u, 1u, 8192u);
    double best = 1e30, sum = 0.0;
    for (uint32_t i = 0u; i < iters; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t c = 0u; c < chain; ++c)
        {
            e.glDispatchCompute_(groups, 1u, 1u);
            e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                               GL178_TEXTURE_UPDATE_BARRIER_BIT);
        }
        glFinish();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        sum += ms;
    }

    // Prove the kernel really ran and was not eliminated: a lane that executed writes something
    // other than the 0xa5a5a5a5 fill. A bench whose body was optimised away reports a huge
    // throughput and means nothing (G457: an arm that was never selected proves nothing).
    std::vector<uint32_t> out(static_cast<size_t>(G542_IMAGE_WIDTH) * outHeight);
    glBindTexture(GL_TEXTURE_2D, outTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, out.data());
    const GLenum err = glGetError();
    uint64_t live = 0ull;
    for (size_t i = 0; i < outWords && i < out.size(); ++i)
        if (out[i] != 0xa5a5a5a5u)
            ++live;

    // Lane-pair-steps: every lane executes the body `repeat` times, and the body is
    // kG607BodyPairs VU pairs. That is the unit G606 and the CPU denominator are both quoted in.
    const double lanes = static_cast<double>(groups) * static_cast<double>(width);
    const double pairSteps = lanes * static_cast<double>(repeat) *
                             static_cast<double>(kG607BodyPairs) * static_cast<double>(chain);
    const double perSec = best > 0.0 ? pairSteps / (best / 1000.0) : 0.0;
    const double usPerDispatch = (best * 1000.0) / static_cast<double>(chain);

    std::fprintf(stderr,
                 "[G607:tbench] arm=%s width=%u groups=%u repeat=%u chain=%u fill=%u "
                 "bodyPairs=%u lanes=%.0f pairSteps=%.0f best=%.3f ms mean=%.3f ms => "
                 "%.2f M pair-steps/s | %.2f us/dispatch | live=%llu/%zu gl=0x%x\n",
                 exact ? "exact" : "native", width, groups, repeat, chain, fillQw,
                 kG607BodyPairs, lanes, pairSteps, best, sum / (double)iters, perSec / 1.0e6,
                 usPerDispatch, (unsigned long long)live, outWords, (unsigned)err);
    // The two denominators every reader needs beside that number, so nobody has to go and look
    // them up before deciding whether it is good.
    std::fprintf(stderr,
                 "[G607:tbench] denominators: CPU one core = 73 M pair-steps/s (13.2-14.4 ns/pair,"
                 " [G483:runprof] clean) | G606 interpreter uniform ceiling = 59.9 M | G606 "
                 "interpreter divergent = 3.09 M | ridepod needs 1.97 M pairs/f\n");

    f.glUseProgram_(0u);
    glDeleteTextures(1, &outTex);
    f.glDeleteProgram_(program);
    return true;
}
