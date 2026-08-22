// ===== G636: ARBITRATION BETWEEN THE TWO GPU AUTHORITIES ======================================
//
// ⭐⭐⭐ THE ROOT CAUSE OF BOTH MEASURED G629-G634 DEFECTS.
//
// This port has TWO independent GPU-side owners of the same 4 MiB of GS memory:
//
//   * **G261 residency** — the native renderer's per-`fbp` FBOs. A draw the native path accepts is
//     rasterised into `m_fbos[fbp]` and its pixels exist ONLY there until `g261Materialize`
//     publishes them back into guest VRAM. While `g_g261Res[t].dirty` is set, neither guest VRAM
//     nor anything else holds those pixels.
//   * **G630 residency** — the persistent raw R32UI image. A batch the native path REFUSED is
//     replayed into the raw image, and its pages are flagged `g_g630GpuDirty`.
//
// G630/G633/G634 never asked the first owner anything. Every one of their direct views
// (`g630PrepareView`, `g630TryT8View`, `g634TryRawView`) is built out of
// `raw image ∪ guest VRAM for non-authoritative pages` — a union that BY CONSTRUCTION excludes
// every pixel a dirty G261 target is still holding. So:
//
//   * **Defect A (G630, `ridepod` chassis renders black).** `g630PrepareView` decodes a row band
//     out of the raw image and writes it into `ensureFbo(p.fbp, ...)` — *the very FBO the native
//     renderer draws into*, because `m_fbos` is keyed by `fbp` alone. Any native content in that
//     band that the raw image does not contain is destroyed, and what replaces it is whatever the
//     replay/seed left there. That is the missing chassis.
//   * **Defect B (G634, the `ridepod` environment renders ~0.63-0.79x).** `g634TryRawView`
//     `continue`s out of the texture pre-pass BEFORE the per-target loop that materialises a dirty
//     resident target overlapping the read, so a texture whose live pixels are in an FBO is decoded
//     from the stale copy instead.
//
// Both are the SAME hole and get the SAME repair: a G630-family direct view may only serve a range
// that **no dirty G261 resident target overlaps**. Otherwise it declines and the draw falls through
// to the shipped path, which materialises that target first (`kG261MatTexAlias`) and then decides
// again — at which point the range is representable and the view is exact.
//
// This is not a per-screen fix: it is one predicate on the single boundary where the two authorities
// meet, evaluated for every consumer of both, with no knowledge of any particular surface.
//
// A second, independent hole in the same family, repaired here:
// `g630PrepareView` PATCHES the pages of the sampled texture range but DECODES the whole
// `[originY, originY+rows)` row band into the FBO. Rows outside the sampled range are therefore
// written from raw-image content nobody proved current. The band is what is written, so the band is
// what must be patched (`g636AppendSurfaceBandPages`).
//
// ⭐⭐⭐ AND THE THIRD HOLE, WHICH IS DEFECT A ITSELF.
// The native renderer seeds a resident FBO by PACKING GUEST VRAM (`uploadFb` ->
// `g419PackFbRows`). Its only staleness guard is the page-generation test at
// `rasterizer_vram_materialization.inc`'s "a GPU-dirty target must NEVER be overwritten by an
// uploadFb from (stale) VRAM" — and that guard detects a **CPU writer**, because a CPU writer bumps
// `g_g178PageGen`. **A G630-authoritative page is stale in guest VRAM with its generation
// UNCHANGED**, so the guard is structurally blind to it and the FBO is seeded from bytes the GPU
// replay already superseded. That is the `ridepod` chassis rendering black. `g636` publishes the
// raw image for the target's rect before the decision is taken (`DC2_G636_NO_FBSEED=1` rolls back).
//
// The mirror of it, repaired the same way: `g261Materialize` writes the FBO back over guest VRAM,
// and a later G630 publication then overwrites that with the OLDER raw content. Settle G630's
// authority first (`DC2_G636_NO_MATPUB=1` rolls back).
//
// Flags:
//   DC2_G636_NO_ARBITRATE=1   rollback — restores the G634-era behaviour verbatim (defects included)
//   DC2_G636_NO_BANDPATCH=1   rollback — patch only the sampled range again
//   DC2_G636_NO_FBSEED=1      rollback — let `uploadFb` seed the FBO from unsettled guest VRAM
//   DC2_G636_NO_MATPUB=1      rollback — let `g261Materialize` publish under live G630 authority
//   DC2_G636_NO_CT32VIEW=1    bisect   — refuse `g630PrepareView` outright
//   DC2_G636_NO_T8VIEW=1      bisect   — refuse the raw PSMT8 view outright
//   DC2_G636_NO_RAWVIEW=1     bisect   — refuse G634's raw CT32/CT24 texture view outright
//   DC2_G636_STAT=1           census   — `[G636:arb]`
//
// The arms and counters live in `g636_arb_flags.inc`, included early enough for the two repair
// sites that sit above this file.

// TRUE when some dirty G261 resident target still owns pixels inside `r`, i.e. when the raw image
// plus guest VRAM is NOT a complete description of that range.
//
// Fail-closed twice over: an invalid range and an invalid residency range both answer "yes", because
// neither can be proven disjoint. The cheap global gate (`g261WaveOn() && g261AnyDirty()`) is what
// keeps this at a couple of predicates on the overwhelmingly common no-residency case.
static bool g636G261DirtyOverlaps(const G144BlockRange &r)
{
    if (s_g636NoArb || !g630DomainOn()) return false;
    if (s_g636Stat) g_g636ArbTests.fetch_add(1u, std::memory_order_relaxed);
    if (!g261WaveOn() || !g261AnyDirty()) return false;
    if (!r.valid)
    {
        if (s_g636Stat) g_g636ArbHits.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &res = g_g261Res[t];
        if (!res.dirty) continue;
        if (!res.range.valid || g144RangeOverlaps(res.range, r))
        {
            if (s_g636Stat) g_g636ArbHits.fetch_add(1u, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

// The block range of the CT32 row band `[rowLo, rowHi]` of a `fbp`/`fbw` surface, expressed the way
// the rest of the domain expresses ranges. One GS CT32 page is 64x32 texels, so the band occupies
// the contiguous page run `[fbp + (rowLo>>5)*fbw, fbp + (rowHi>>5)*fbw + fbw)`.
static G144BlockRange g636SurfaceBandRange(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi)
{
    G144BlockRange out{};
    if (fbw == 0u || rowHi < rowLo) return out;
    const uint32_t firstPage = fbp + (static_cast<uint32_t>(rowLo) >> 5u) * fbw;
    const uint32_t lastPage = fbp + (static_cast<uint32_t>(rowHi) >> 5u) * fbw + (fbw - 1u);
    if (firstPage >= kG630Pages) return out;
    out.valid = true;
    out.first = firstPage << 5u;
    out.last = (std::min(kG630Pages - 1u, lastPage) << 5u) + 31u;
    return out;
}

// Append EVERY guest page under the decoded row band, not merely the pages of the sampled texture
// range. `runG630ViewOnWorker` writes the whole band into the FBO, so anything less leaves rows in
// that FBO decoded from raw-image content that was never reconciled with guest VRAM.
static void g636AppendSurfaceBandPages(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi,
                                       std::vector<uint32_t> &pages,
                                       std::vector<uint32_t> &words)
{
    if (fbw == 0u || rowHi < rowLo) return;
    const uint32_t firstPage = fbp + (static_cast<uint32_t>(rowLo) >> 5u) * fbw;
    const uint32_t lastPage = fbp + (static_cast<uint32_t>(rowHi) >> 5u) * fbw + (fbw - 1u);
    if (firstPage >= kG630Pages || lastPage < firstPage) return;
    const size_t before = pages.size();
    g630AppendGuestPages(firstPage, lastPage - firstPage + 1u, pages, words);
    if (s_g636Stat)
    {
        g_g636BandPages.fetch_add(pages.size() - before, std::memory_order_relaxed);
        g_g636BandCalls.fetch_add(1u, std::memory_order_relaxed);
    }
}

// The generation of the decoded band, so a view is re-built when ANY page it writes moved, not only
// when a page under the sampled window moved.
static uint64_t g636SurfaceBandGen(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi)
{
    if (fbw == 0u || rowHi < rowLo) return 0u;
    const uint32_t firstPage = fbp + (static_cast<uint32_t>(rowLo) >> 5u) * fbw;
    const uint32_t lastPage =
        std::min(kG630Pages - 1u, fbp + (static_cast<uint32_t>(rowHi) >> 5u) * fbw + (fbw - 1u));
    uint64_t sum = 0u;
    for (uint32_t p = firstPage; p <= lastPage && p < kG630Pages; ++p)
        sum += g_g178PageGen[p].load(std::memory_order_relaxed);
    return sum;
}

static void g636Report()
{
    if (!s_g636Stat) return;
    std::fprintf(stderr,
                 "[G636:arb] arb=%u bandPatch=%u fbSeed=%u matPub=%u tests=%llu hits=%llu | "
                 "refused ct32=%llu t8=%llu raw=%llu | bandCalls=%llu bandPages=%llu "
                 "seedPub=%llu matPub=%llu\n",
                 s_g636NoArb ? 0u : 1u, s_g636NoBandPatch ? 0u : 1u,
                 s_g636NoFbSeed ? 0u : 1u, s_g636NoMatPub ? 0u : 1u,
                 (unsigned long long)g_g636ArbTests.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ArbHits.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ArbCt32.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ArbT8.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ArbRaw.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636BandCalls.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636BandPages.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636FbSeed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636MatPub.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G636:zmirror] residentZ=%u shadow=%u reseedArm=%u refusedDepthBatches=%llu "
                 "seeds=%llu staleDetected=%llu unpublishedAtStale=%llu\n",
                 s_g636ResidentZ ? 1u : 0u,
                 s_g636NoZShadow ? 0u : 1u, s_g636ZReseed ? 1u : 0u,
                 (unsigned long long)g_g636ZRefused.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ZSeeds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ZStale.load(std::memory_order_relaxed),
                 (unsigned long long)g_g636ZLost.load(std::memory_order_relaxed));
    // ⭐ THE ARITHMETIC THAT DECIDES THE ARC. `keptWall` is the CPU band replay's pole-thread wait
    // that a GPU backend could still delete; `refusedZWall` is the part the private-depth removal
    // put permanently out of reach. Compare `keptWall / presents` against the measured transport.
    const double refusedMs = (double)g_g636ZRefusedWallNs.load(std::memory_order_relaxed) / 1e6;
    const double keptMs = (double)g_g636ZKeptWallNs.load(std::memory_order_relaxed) / 1e6;
    const double totalMs = refusedMs + keptMs;
    std::fprintf(stderr,
                 "[G636:wall] poolWall total=%.1fms | refusedForZ=%.1fms (%.1f%%) "
                 "addressable=%.1fms (%.1f%%)\n",
                 totalMs, refusedMs, totalMs > 0.0 ? 100.0 * refusedMs / totalMs : 0.0,
                 keptMs, totalMs > 0.0 ? 100.0 * keptMs / totalMs : 0.0);
}
