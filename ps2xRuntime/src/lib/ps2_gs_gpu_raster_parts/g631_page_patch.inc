// ===== G631: RAW-VRAM PAGE PATCH UPLOAD — RUN-COALESCED ======================================
//
// G630 keeps one persistent 4 MiB R32UI raw GS-VRAM image whose ROW p holds guest page p
// (2048 words = 8 KiB). Three sites patch guest-authoritative pages into it before a GPU consumer
// reads them: the resident replay (`runG629OnWorker`), the CT32 raw->linear view
// (`runG630ViewOnWorker`) and the raw PSMT8 decode (`runG630T8OnWorker`).
//
// ⭐ ALL THREE ISSUED ONE `glTexSubImage2D` PER PAGE. On `ridepod`'s hot `fbp=0x142` surface the
// patch set is the whole 640x416 target — 128 pages — so one view cost 128 separate 8 KiB texture
// sub-uploads. That loop, not the view compute shader, is what G630 measured as `630view=108.6
// ms/f` at ONE item per frame; the shader itself dispatches 266k invocations.
//
// The pages arrive ASCENDING (both producers walk a page range upward), and a contiguous page run
// is ALSO contiguous in the flat `patchWords` array — 2048 words per entry, same order. So a run of
// k consecutive pages starting at index i is exactly `glTexSubImage2D(x=0, y=pages[i], w=2048,
// h=k, words + i*2048)`: identical bytes into identical texels, one driver call instead of k.
// 128 -> 1 on the hot view.
//
// Exactness is by construction: the coalesced arm writes the same texels from the same source
// words in the same order. GL_UNPACK_ALIGNMENT is 1 (set once at backend init) and
// GL_UNPACK_ROW_LENGTH is 0 outside the G309 composite, so a multi-row upload is densely packed.
//
// Rollback: DC2_G631_NO_PATCHRUN=1 restores the per-page loop verbatim.
// Census:   DC2_G631_SPLIT=1 prints per-site pages/calls/ms so the patch half of a view can be
//           read separately from its dispatch half.

static bool g631PatchRunOn()
{
    static const bool on = (std::getenv("DC2_G631_NO_PATCHRUN") == nullptr);
    return on;
}

static bool g631SplitOn()
{
    static const bool on = (std::getenv("DC2_G631_SPLIT") != nullptr);
    return on;
}

// DC2_G631_VERIFY=1 — the same-run oracle for BOTH mechanisms this phase changed:
//   * every coalesced patch is read back out of the raw image and compared word-for-word against
//     the words that were uploaded (proves the multi-row sub-upload lands on the same texels as
//     the per-page form);
//   * every band readback is re-read through the legacy whole-image `glGetTexImage` and the band's
//     own rows compared (proves the FBO/glReadPixels origin and offset match the texture rows).
// Both denominators are printed, so a "clean" verdict cannot be `bad=0` on `checks=0`.
static bool g631VerifyOn()
{
    static const bool on = (std::getenv("DC2_G631_VERIFY") != nullptr);
    return on;
}

static uint64_t g_g631PatchChecks = 0ull, g_g631PatchBad = 0ull;
static uint64_t g_g631ReadChecks = 0ull, g_g631ReadBad = 0ull;

static void g631ReportVerify()
{
    if (!g631VerifyOn())
        return;
    std::fprintf(stderr,
                 "[G631:verify] patch checks=%llu bad=%llu | bandread checks=%llu bad=%llu\n",
                 (unsigned long long)g_g631PatchChecks, (unsigned long long)g_g631PatchBad,
                 (unsigned long long)g_g631ReadChecks, (unsigned long long)g_g631ReadBad);
    std::fflush(stderr);
}

enum
{
    kG631SiteView = 0,
    kG631SiteReplay = 1,
    kG631SiteT8 = 2,
    kG631SiteN = 3
};

static const char *const kG631SiteName[kG631SiteN] = {"view", "replay", "t8"};

struct G631PatchRow
{
    uint64_t items = 0u, pages = 0u, calls = 0u;
    double patchMs = 0.0, otherMs = 0.0;
};
static G631PatchRow g_g631Rows[kG631SiteN];
static uint64_t g_g631Items = 0ull;

// Worker-thread only (every caller runs inside the single GL worker), so plain scalars.
static void g631NotePatch(int site, uint64_t pages, uint64_t calls, double patchMs, double otherMs)
{
    if (!g631SplitOn() || site < 0 || site >= kG631SiteN)
        return;
    G631PatchRow &r = g_g631Rows[site];
    ++r.items;
    r.pages += pages;
    r.calls += calls;
    r.patchMs += patchMs;
    r.otherMs += otherMs;
    if ((++g_g631Items % 256ull) != 0ull)
        return;
    for (int i = 0; i < kG631SiteN; ++i)
    {
        const G631PatchRow &q = g_g631Rows[i];
        if (q.items == 0u)
            continue;
        std::fprintf(stderr,
                     "[G631:patch] site=%-6s run=%u items=%llu pages=%llu calls=%llu "
                     "pages/item=%.1f calls/item=%.1f patch=%.3f other=%.3f ms/item "
                     "(patch tot=%.1fms)\n",
                     kG631SiteName[i], g631PatchRunOn() ? 1u : 0u,
                     (unsigned long long)q.items, (unsigned long long)q.pages,
                     (unsigned long long)q.calls,
                     (double)q.pages / (double)q.items,
                     (double)q.calls / (double)q.items,
                     q.patchMs / (double)q.items, q.otherMs / (double)q.items,
                     q.patchMs);
    }
    std::fflush(stderr);
}

// Uploads `pages`/`words` into the persistent raw-VRAM texture. `calls` receives the number of
// glTexSubImage2D calls actually issued, so a census can prove the two arms differ.
// Returns false on an out-of-range page (fail closed — the caller refuses the item).
static bool g631UploadPatchPages(GLuint tex,
                                 const std::vector<uint32_t> &pages,
                                 const std::vector<uint32_t> &words,
                                 uint64_t &calls)
{
    calls = 0u;
    if (words.size() != pages.size() * 2048u)
        return false;
    if (pages.empty())
        return true;
    glBindTexture(GL_TEXTURE_2D, tex);
    const bool coalesce = g631PatchRunOn();
    size_t i = 0u;
    while (i < pages.size())
    {
        if (pages[i] >= 512u)
            return false;
        size_t j = i + 1u;
        if (coalesce)
            while (j < pages.size() && pages[j] < 512u && pages[j] == pages[j - 1u] + 1u)
                ++j;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(pages[i]),
                        2048, static_cast<GLsizei>(j - i),
                        GL541_RED_INTEGER, GL_UNSIGNED_INT,
                        words.data() + i * 2048u);
        ++calls;
        i = j;
    }
    if (g631VerifyOn())
    {
        // Read the WHOLE image back and compare only the rows this call claimed to write. The
        // texture is 2048 x 512 R32UI and row p is guest page p, so page `pages[k]` must equal
        // `words[k*2048 .. )` exactly.
        static std::vector<uint32_t> s_full;
        s_full.assign(2048u * 512u, 0u);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL541_RED_INTEGER, GL_UNSIGNED_INT, s_full.data());
        if (glGetError() == GL_NO_ERROR)
        {
            for (size_t k = 0; k < pages.size(); ++k)
            {
                const uint32_t *have = s_full.data() + static_cast<size_t>(pages[k]) * 2048u;
                const uint32_t *want = words.data() + k * 2048u;
                for (uint32_t w = 0u; w < 2048u; ++w)
                {
                    ++g_g631PatchChecks;
                    if (have[w] != want[w])
                    {
                        if (++g_g631PatchBad <= 8ull)
                            std::fprintf(stderr,
                                         "[G631:patchbad] page=%u word=%u have=%08x want=%08x\n",
                                         pages[k], w, have[w], want[w]);
                    }
                }
            }
        }
        g631ReportVerify();
    }
    return true;
}
