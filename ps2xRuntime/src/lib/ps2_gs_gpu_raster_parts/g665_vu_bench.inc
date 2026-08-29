// G665 - translated GPU VU1 backend: corpus replay, EXACTNESS ORACLE, and throughput.
//
// Modelled on g606_wide_vu.inc, and deliberately reusing its oracle rather than writing a new one:
// `g543CompareSection` over postState / postPipe / postData / packetDesc / packetBytes is the same
// promotion-grade comparison G543 was accepted against, so a translated program is held to exactly
// the standard the interpreter already meets. A new backend does not get a new bar.
//
// ⚠️ ONE PROGRAM IS SPECIALIZED TO ONE CODE IMAGE. A corpus can hold kicks from several
// microprograms, and running a record against a program translated from a DIFFERENT image would
// produce a mismatch that means nothing about the translator. So the bench groups records by
// `g665CodeImageKey`, translates the dominant image, and executes ONLY its records - reporting the
// coverage explicitly so the denominator is never silent (Rule 25).
//
// This is the shape a production cache takes: key on the code image, compile once, reuse.

#if defined(PS2X_G665_GPUVU)

static bool g665RunTranslatedBench(GLFns &f, G178GLExt &e)
{
    if (e.glDispatchCompute_ == nullptr || e.glBindImageTexture_ == nullptr ||
        e.glMemoryBarrier_ == nullptr)
    {
        std::fprintf(stderr, "[G665:tvu] FAIL compute entry points unavailable; CPU path unchanged\n");
        return false;
    }

    const char *envPath = std::getenv("DC2_G665_CORPUS_PATH");
    if (envPath == nullptr || envPath[0] == '\0')
        envPath = std::getenv("DC2_G606_CORPUS_PATH");
    if (envPath == nullptr || envPath[0] == '\0')
        envPath = std::getenv("DC2_G543_CORPUS_PATH");
    const std::filesystem::path path = envPath && envPath[0] != '\0'
        ? std::filesystem::path(envPath)
        : std::filesystem::path("captures/g541_vu1_corpus.bin");

    G542AbiCorpus corpus;
    std::string why;
    if (!g542LoadAbiCorpus(path, corpus, why) || !g543ValidateLayout(corpus, why))
    {
        std::fprintf(stderr, "[G665:tvu] FAIL path='%s': %s; CPU path unchanged\n",
                     path.string().c_str(), why.c_str());
        return false;
    }

    for (uint32_t r = 0u; r < corpus.records; ++r)
        corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 31u] =
            static_cast<uint32_t>(corpus.payload.size() +
                                  static_cast<size_t>(r) * G543_STATUS_WORDS);

    // ---- group records by code image -------------------------------------------------------
    // A linear scan, not a hash map: real corpora hold a handful of distinct microprograms (G607
    // measured the dominant image at 98.6% of kicks), so an O(records x images) pass is trivial and
    // avoids pulling <unordered_map> in at a point where a standard header cannot be included.
    std::vector<uint64_t> keys(corpus.records, 0ull);
    std::vector<uint64_t> imageKeys;
    std::vector<uint32_t> imageCounts;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        const uint32_t codeByte = corpus.commands[static_cast<size_t>(r) * G542_ABI_WORDS + 19u];
        if ((codeByte & 3u) != 0u ||
            (codeByte >> 2u) + G665_CODE_WORDS > corpus.payload.size())
        {
            std::fprintf(stderr, "[G665:tvu] FAIL record %u code image outside payload\n", r);
            return false;
        }
        keys[r] = g665CodeImageKey(corpus.payload.data() + (codeByte >> 2u));
        size_t slot = 0u;
        for (; slot < imageKeys.size(); ++slot)
            if (imageKeys[slot] == keys[r]) break;
        if (slot == imageKeys.size())
        {
            imageKeys.push_back(keys[r]);
            imageCounts.push_back(0u);
        }
        ++imageCounts[slot];
    }
    uint64_t domKey = 0ull;
    uint32_t domCount = 0u;
    for (size_t i = 0u; i < imageKeys.size(); ++i)
        if (imageCounts[i] > domCount) { domCount = imageCounts[i]; domKey = imageKeys[i]; }

    uint32_t domFirst = 0u;
    for (uint32_t r = 0u; r < corpus.records; ++r)
        if (keys[r] == domKey) { domFirst = r; break; }
    const uint32_t domCodeByte =
        corpus.commands[static_cast<size_t>(domFirst) * G542_ABI_WORDS + 19u];

    const uint32_t local = g606EnvU32("DC2_G665_LOCAL", 1u, 1u, 1024u);
    const uint32_t iters = g606EnvU32("DC2_G665_ITERS", 8u, 1u, 64u);
    // ⚠️ Default is a BOUND, not "all". Specializing all 1,620 pairs of the `fight` image did not
    // finish linking in 420 s on a GTX 1060 (see g665_vu_translate.inc). Unbounded is available as
    // DC2_G665_MAXPAIRS=2048 for anyone re-measuring that wall on a different driver.
    const uint32_t maxPairs = g606EnvU32("DC2_G665_MAXPAIRS", 128u, 0u, 2048u);
    // DC2_G665_NOFALLBACK=1 removes the in-shader interpreter from the PROGRAM (see the translator).
    // Only valid when the specialized set covers every executed PC; `interp=` below is the proof.
    const bool fallback = !g158EnvFlag("DC2_G665_NOFALLBACK");

    // Two extra words per record, appended after the status area, for the specialized/interpreted
    // pair counters. Without them a `bad=0` says nothing about whether the translation ever ran.
    const uint32_t payloadWords = static_cast<uint32_t>(corpus.payload.size());
    const size_t counterBase =
        corpus.payload.size() + static_cast<size_t>(corpus.records) * G543_STATUS_WORDS;
    const size_t histBase = counterBase + static_cast<size_t>(corpus.records) * 2u;
    const size_t outputWords = histBase + G665_PAIRS;

    // ---- PASS 1: profile ---------------------------------------------------------------------
    // Run the pure interpreter with a per-PC histogram, so pass 2 can specialize the pairs that
    // actually execute rather than the first N in PC order (which measured 0.36% coverage).
    std::vector<uint32_t> hot;
    if (maxPairs != 0u)
    {
        G665TranslateStats pst;
        const std::string psrc =
            g665TranslateProgram(corpus.payload.data() + (domCodeByte >> 2u), local, 0u, true,
                                 payloadWords, corpus.records, true, nullptr, pst, why);
        GLuint pprog = 0u;
        if (!psrc.empty() && g606LinkProgram(f, psrc, pprog, why))
        {
            G542GlObjects pgl;
            pgl.f = &f;
            uint32_t h0 = 0u, h1 = 0u, h2 = 0u;
            std::vector<uint32_t> pinit(outputWords, 0u);
            if (g542CreateImage(pgl.textures[0], corpus.commands, h0, why) &&
                g542CreateImage(pgl.textures[1], corpus.payload, h1, why) &&
                g542CreateImage(pgl.textures[2], pinit, h2, why))
            {
                f.glUseProgram_(pprog);
                e.glBindImageTexture_(2u, pgl.textures[0], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
                e.glBindImageTexture_(3u, pgl.textures[1], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
                e.glBindImageTexture_(4u, pgl.textures[2], 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);
                e.glDispatchCompute_((corpus.records + local - 1u) / local, 1u, 1u);
                e.glMemoryBarrier_(GL178_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                   GL178_TEXTURE_UPDATE_BARRIER_BIT);
                glFinish();
                std::vector<uint32_t> pout(static_cast<size_t>(G542_IMAGE_WIDTH) * h2);
                glBindTexture(GL_TEXTURE_2D, pgl.textures[2]);
                glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, pout.data());
                if (glGetError() == GL_NO_ERROR && histBase + G665_PAIRS <= pout.size())
                {
                    std::vector<std::pair<uint32_t, uint32_t>> rank;
                    uint64_t total = 0ull;
                    for (uint32_t p = 0u; p < G665_PAIRS; ++p)
                    {
                        const uint32_t n = pout[histBase + p];
                        total += n;
                        if (n != 0u)
                            rank.emplace_back(n, p);
                    }
                    std::sort(rank.begin(), rank.end(),
                              [](const std::pair<uint32_t, uint32_t> &a,
                                 const std::pair<uint32_t, uint32_t> &b) {
                                  return a.first > b.first;
                              });
                    uint64_t covered = 0ull;
                    for (size_t i = 0u; i < rank.size() && hot.size() < maxPairs; ++i)
                    {
                        hot.push_back(rank[i].second);
                        covered += rank[i].first;
                    }
                    std::fprintf(stderr,
                                 "[G665:tvu] profile livePcs=%zu totalPairs=%llu | top%zu covers "
                                 "%.2f%% of executed pairs\n",
                                 rank.size(), static_cast<unsigned long long>(total), hot.size(),
                                 total ? 100.0 * static_cast<double>(covered) /
                                             static_cast<double>(total) : 0.0);
                }
            }
        }
        else
        {
            std::fprintf(stderr, "[G665:tvu] profile pass FAILED (%s); falling back to PC order\n",
                         why.c_str());
        }
    }

    // ---- PASS 2: profile-guided translation ---------------------------------------------------
    G665TranslateStats st;
    const auto tr0 = std::chrono::steady_clock::now();
    const std::string source =
        g665TranslateProgram(corpus.payload.data() + (domCodeByte >> 2u), local, maxPairs,
                             fallback, payloadWords, corpus.records, false,
                             hot.empty() ? nullptr : &hot, st, why);
    const double translateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr0).count();
    if (source.empty())
    {
        std::fprintf(stderr, "[G665:tvu] FAIL translate: %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    // Report the translation BEFORE linking: a driver that dies on a 4 MB shader should still leave
    // the numbers that explain why (Rule 25 - print keyed to the denominator, not to success).
    std::fprintf(stderr,
                 "[G665:tvu] image=%016llx records=%u/%u (%.2f%%) images=%zu | "
                 "pairs emitted=%u skipped=%u maxPairs=%u | glsl=%.2f MB translateMs=%.1f local=%u\n",
                 static_cast<unsigned long long>(domKey), domCount, corpus.records,
                 corpus.records ? 100.0 * domCount / corpus.records : 0.0,
                 imageKeys.size(), st.emitted, st.skipped, maxPairs,
                 st.sourceBytes / (1024.0 * 1024.0), translateMs, local);

    G542GlObjects gl;
    gl.f = &f;
    const auto lk0 = std::chrono::steady_clock::now();
    const bool linked = g606LinkProgram(f, source, gl.program, why);
    const double linkMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lk0).count();
    std::fprintf(stderr, "[G665:tvu] link=%s linkMs=%.1f\n", linked ? "OK" : "FAIL", linkMs);
    if (!linked)
    {
        std::fprintf(stderr, "[G665:tvu] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {}
    uint32_t commandHeight = 0u, payloadHeight = 0u, outputHeight = 0u;
    std::vector<uint32_t> outputInit(outputWords, 0xa5a5a5a5u);
    if (!g542CreateImage(gl.textures[0], corpus.commands, commandHeight, why) ||
        !g542CreateImage(gl.textures[1], corpus.payload, payloadHeight, why) ||
        !g542CreateImage(gl.textures[2], outputInit, outputHeight, why))
    {
        std::fprintf(stderr, "[G665:tvu] FAIL %s; CPU path unchanged\n", why.c_str());
        return false;
    }

    f.glUseProgram_(gl.program);
    e.glBindImageTexture_(2u, gl.textures[0], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(3u, gl.textures[1], 0, GL_FALSE, 0, GL542_READ_ONLY, GL541_R32UI);
    e.glBindImageTexture_(4u, gl.textures[2], 0, GL_FALSE, 0, GL178_READ_WRITE, GL541_R32UI);

    const uint32_t groups = (corpus.records + local - 1u) / local;

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
        std::fprintf(stderr, "[G665:tvu] FAIL readback gl=0x%x; CPU path unchanged\n",
                     static_cast<unsigned>(err));
        return false;
    }

    // ---- exactness: G543's own promotion-grade section comparison ---------------------------
    uint32_t bad = 0u, checked = 0u;
    uint64_t pairs = 0ull, specPairs = 0ull, interpPairs = 0ull;
    G543Mismatch firstMismatch;
    for (uint32_t r = 0u; r < corpus.records; ++r)
    {
        if (keys[r] != domKey)
            continue;   // executed against a different image; not this program's result
        const size_t c = static_cast<size_t>(r) * G542_ABI_WORDS;
        const uint32_t *cmd = corpus.commands.data() + c;
        const size_t s = corpus.commands[c + 31u];
        pairs += cmd[10];
        ++checked;
        const size_t ctr = counterBase + static_cast<size_t>(r) * 2u;
        if (ctr + 1u < output.size())
        {
            specPairs += output[ctr];
            interpPairs += output[ctr + 1u];
        }
        if (s + G543_STATUS_WORDS > output.size() || output[s] != G543_STATUS_MAGIC ||
            output[s + 1u] != 0u)
        {
            if (bad++ == 0u)
                std::fprintf(stderr,
                             "[G665:tvu] STATUS record=%u magic=%08x flags=%08x pc=%04x word=%08x "
                             "cycles=%u/%u stop=%u/%u\n",
                             r, s < output.size() ? output[s] : 0u,
                             s + 1u < output.size() ? output[s + 1u] : 0xffffffffu,
                             s + 2u < output.size() ? output[s + 2u] : 0xffffffffu,
                             s + 3u < output.size() ? output[s + 3u] : 0u,
                             s + 4u < output.size() ? output[s + 4u] : 0u, cmd[10],
                             s + 5u < output.size() ? output[s + 5u] : 0u, cmd[12]);
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

    const double meanMs = sum / static_cast<double>(iters);
    const double usPerKick = best * 1000.0 / static_cast<double>(checked ? checked : 1u);
    const double pairsPerSec = best > 0.0 ? static_cast<double>(pairs) / (best / 1000.0) : 0.0;
    // ⭐ `spec`/`interp` are the DENOMINATOR that makes `bad` mean something: they say how many
    // executed pairs actually went through a translated case. A clean oracle with spec=0 proves
    // only that the interpreter still works.
    const double specShare = (specPairs + interpPairs) != 0ull
        ? 100.0 * static_cast<double>(specPairs) /
              static_cast<double>(specPairs + interpPairs)
        : 0.0;
    std::fprintf(stderr,
                 "[G665:tvu] local=%u groups=%u checked=%u pairs=%llu iters=%u fallback=%d | "
                 "bestMs=%.3f meanMs=%.3f | usPerKick=%.3f pairsPerSec=%.3fM | "
                 "spec=%llu interp=%llu specShare=%.2f%% | bad=%u\n",
                 local, groups, checked, static_cast<unsigned long long>(pairs), iters,
                 fallback ? 1 : 0, best, meanMs, usPerKick, pairsPerSec / 1.0e6,
                 static_cast<unsigned long long>(specPairs),
                 static_cast<unsigned long long>(interpPairs), specShare, bad);
    if (bad != 0u && firstMismatch.record != 0xffffffffu)
        std::fprintf(stderr,
                     "[G665:tvu] MISMATCH record=%u section=%s byte=%u expected=%02x actual=%02x "
                     "expectedWord=%08x actualWord=%08x\n",
                     firstMismatch.record, firstMismatch.section, firstMismatch.byte,
                     firstMismatch.expected, firstMismatch.actual,
                     firstMismatch.expectedWord, firstMismatch.actualWord);
    return bad == 0u;
}

#endif // PS2X_G665_GPUVU
