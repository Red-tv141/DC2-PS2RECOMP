// ===== G599: the 512-TEXEL-WIDE PSMT8 VIEW (default OFF, DC2_G599_T8WIDE=1) =====
// `texW > 256` rejects the consumer that actually paints the MAP-15 sky band. `[G599:src]` — the
// census taken at the one point where a read's source is final — shows the display targets
// (dst=0x0/0x68) sampling `tbp=0x2720` as **PSMT8 512x512 with tbw=8**, 475 reads in the window,
// every one of them `src=0x0` (guest VRAM) while the CT32 reads of the SAME base are all served
// from an FBO (`src=0xfffffff5`). The 256 cap is bring-up narrowing from the tuples G281 happened
// to measure — the byte map below proves per texel that the page is in range, page-aligned, inside
// covLo..covHi and owned by no other dirty target, and that proof does not care how wide the view
// is. The packing already carries 9 bits of source X and 9 of GL-Y, so a 512-wide CT32 source is
// representable without any format change.
//
// Default-OFF until it is gated; `DC2_G599_NO_T8WIDE=1` is reserved as the rollback if promoted.
static bool g599T8WideOn()
{
    static const bool on = !envFlagEnabled("DC2_G599_NO_T8WIDE") &&
                           envFlagEnabled("DC2_G599_T8WIDE");
    return on;
}
static int g599T8MaxW()
{
    return g599T8WideOn() ? 512 : 256;
}

// Build the exact per-texel source map for the measured PSMT8 views. Each RGBA8 map texel packs:
// R=src X low byte, G=src GL-Y low byte, B.bit0=GL-Y bit8, B.bits1..2=byte lane,
// B.bit3=src X bit8. The source is restricted to one dirty CT32 FBO, and every physical page
// must be wholly outside all other dirty owners.
static bool g281BuildT8MapCold(const G144Entry &e, int srcTi, int texW, int texH,
                               std::vector<uint32_t> &out)
{
    static const G281Ct32Inverse s_inv;
    if (!s_inv.ok || srcTi < 0 || srcTi >= static_cast<int>(kG248TargetCount) ||
        texW <= 0 || texH <= 0 || texW > g599T8MaxW() || texH > 512)
        return false;
    const G261Res &src = g_g261Res[srcTi];
    if (!src.dirty || !src.range.valid || src.fbw == 0u || src.fbW <= 0 || src.fbW > 512)
        return false;
    const uint32_t srcBase = framePageBaseToBlock(kG261Fbp[srcTi]);
    const uint32_t pageBwT8 = std::max<uint32_t>(1u, e.ctx.tex0.tbw >> 1u);
    out.resize(static_cast<size_t>(texW) * texH);
    // ⭐ G599: the page conjuncts (range, alignment, foreign dirty owner) depend only on the PAGE,
    // never on the texel, and this walk visits at most 32 distinct pages — so the shipped per-texel
    // form re-ran a 14-target ownership walk ~8,000 times per page. At the 128x128 views it was
    // built for that is invisible; at the 512x512 view G599 needs it is 262,144 iterations and the
    // frame collapses to 935 ms. Memoize the ACCEPT verdict per page. Exact: the same conjuncts in
    // the same order and the same fail-closed direction — a rejected page still returns false
    // immediately, so only accepts are ever cached.
    constexpr uint32_t kG599PageMemo = 256u;
    uint8_t pageOk[kG599PageMemo] = {};
    for (int vv = 0; vv < texH; ++vv)
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint32_t off = GSMem::AddressP8(
                e.ctx.tex0.tbp0, pageBwT8,
                static_cast<uint32_t>(uu), static_cast<uint32_t>(vv));
            const uint32_t pageBlock = (off >> 13u) << 5u;
            const uint32_t memoIdx = pageBlock >= srcBase ? ((pageBlock - srcBase) >> 5u)
                                                          : kG599PageMemo;
            if (memoIdx >= kG599PageMemo || pageOk[memoIdx] == 0u)
            {
                const G144BlockRange page{pageBlock, pageBlock + 31u, true};
                if (page.last > 0x3FFFu || page.first < src.range.first ||
                    page.last > src.range.last || pageBlock < srcBase ||
                    ((pageBlock - srcBase) & 31u) != 0u)
                    return false;
                for (uint32_t t = 0; t < kG248TargetCount; ++t)
                {
                    if (static_cast<int>(t) == srcTi || !g_g261Res[t].dirty)
                        continue;
                    if (!g_g261Res[t].range.valid ||
                        g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock))
                        return false;
                }
                if (memoIdx < kG599PageMemo)
                    pageOk[memoIdx] = 1u;
            }

            const uint32_t pageOff = (pageBlock - srcBase) >> 5u;
            const uint32_t pageRow = pageOff / src.fbw;
            const uint32_t pageCol = pageOff % src.fbw;
            const uint32_t word = (off & 8191u) >> 2u;
            if (word >= s_inv.x.size())
                return false;
            const int sx = static_cast<int>(pageCol) * 64 + s_inv.x[word];
            const int sy = static_cast<int>(pageRow) * 32 + s_inv.y[word];
            if (sx < 0 || sx >= src.fbW || sy < src.covLo || sy > src.covHi ||
                sy < 0 || sy >= 512)
                return false;
            const uint32_t glY = static_cast<uint32_t>(511 - sy);
            const uint32_t lane = off & 3u;
            const uint32_t packed = (static_cast<uint32_t>(sx) & 0xFFu) |
                                    ((glY & 0xFFu) << 8u) |
                                    ((((glY >> 8u) & 1u) | (lane << 1u) |
                                      (((static_cast<uint32_t>(sx) >> 8u) & 1u) << 3u))
                                     << 16u) |
                                    0xFF000000u;
            out[static_cast<size_t>(vv) * texW + static_cast<size_t>(uu)] = packed;
        }
    return true;
}

// ===== G618: the same map, with the PER-PAGE work lifted out of the PER-TEXEL loop =====
//
// WHY THIS LOOP. The G618 premise gate measured `dungeon1`'s entry/run window (n=241..1081, the
// 60-75 ms/f frames the route was recorded for) at **801.8 `g281` cache MISSES per frame** against
// **0.00/frame** in the steady tail — and a GS-worker PC sample resolves 18.45% of all samples into
// this builder's inner loop plus 3.01% into the out-of-line `GSMem::AddressP8` it calls per texel.
// Capstone on the hottest PC names the mechanism exactly: **6.57% of every sample in the run lands
// on the instruction immediately after `div dword ptr [r13+0x14]`** — the skid of one 32-bit
// HARDWARE DIVIDE executed once per texel for `pageOff / src.fbw` and `pageOff % src.fbw`.
//
// Neither quantity is a function of the texel. `pageRow`/`pageCol` depend only on `pageBlock`, which
// is exactly the key the shipped `pageOk[]` accept-memo is already keyed on — the loop was
// memoizing the ACCEPT VERDICT for a page and then re-deriving that same page's row/column with a
// divide on every one of its texels. Extending the existing memo to carry the two derived terms is
// therefore exact BY CONSTRUCTION: single-threaded, and nothing inside the loop mutates
// `g_g261Res`, so a page's verdict and its terms are both constant for the whole call.
//
// The second lowering removes the cross-TU call. `AddressP8` lives in ps2_gs_memory.cpp, so it can
// never inline here. ⭐ The table is NOT hand-derived from the PSMT8 layout — it is built by
// PROBING THE SHIPPED `GSMem::AddressP8`, exactly the way `G281Ct32Inverse` above probes
// `addrPSMCT32` (g594's law), and then the page decomposition is SELF-TESTED against that same
// shipped function over a grid that crosses pages in both axes and includes non-page-aligned `bp`.
// A failing self-test simply leaves the fast path off forever (fail-closed, like
// `g617ColumnPairsAdjacent`), so a future layout change cannot silently produce wrong texels.
//
// Rollback DC2_G618_NO_FAST_T8MAP=1 · oracle DC2_G618_VERIFY=1 · census DC2_G618_STAT=1.
static bool g618FastT8MapOn()
{
    static const bool on = !envFlagEnabled("DC2_G618_NO_FAST_T8MAP");
    return on;
}
static bool g618T8MapVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G618_VERIFY");
    return on;
}
static bool g618T8MapStatOn()
{
    static const bool on = envFlagEnabled("DC2_G618_STAT");
    return on;
}

// The 128x64 PSMT8 page permutation, probed out of the shipped reader, plus the one-time proof that
// AddressP8(bp,bw,x,y) == bp*256 + ((y>>6)*bw + (x>>7))*8192 + perm[(y&63)*128 + (x&127)].
struct G618P8Page
{
    std::array<uint16_t, 128u * 64u> perm{};
    bool ok = false;
    G618P8Page()
    {
        for (uint32_t yy = 0u; yy < 64u; ++yy)
            for (uint32_t xx = 0u; xx < 128u; ++xx)
            {
                const uint32_t off = GSMem::AddressP8(0u, 1u, xx, yy);
                if (off >= 8192u)
                    return;
                perm[yy * 128u + xx] = static_cast<uint16_t>(off);
            }
        // A page must be a bijection onto its 8192 bytes, or the "one page == one 8 KiB block run"
        // premise the accept-memo rests on is false.
        std::array<uint8_t, 8192> seen{};
        for (const uint16_t v : perm)
        {
            if (seen[v] != 0u)
                return;
            seen[v] = 1u;
        }
        static const uint32_t kBp[] = {0u, 1u, 7u, 32u, 0x2720u, 0x3420u};
        static const uint32_t kBw[] = {1u, 2u, 4u, 8u};
        for (const uint32_t bp : kBp)
            for (const uint32_t bw : kBw)
                for (uint32_t yy = 0u; yy < 128u; ++yy)
                    for (uint32_t xx = 0u; xx < 256u; ++xx)
                    {
                        const uint32_t want = GSMem::AddressP8(bp, bw, xx, yy);
                        const uint32_t got = bp * 256u +
                                             (((yy >> 6u) * bw + (xx >> 7u)) << 13u) +
                                             perm[(yy & 63u) * 128u + (xx & 127u)];
                        if (want != got)
                            return;
                    }
        ok = true;
    }
};

static std::atomic<uint64_t> g_g618Calls{0}, g_g618Texels{0}, g_g618Ns{0};
static std::atomic<uint64_t> g_g618Checks{0}, g_g618Bad{0}, g_g618Decline{0};
// ⭐ THE COVERAGE DENOMINATOR. `g_g618Texels` counts only ACCEPTED builds, and the first oracle run
// showed it frozen at 77,824 while `calls` passed 660,000 — i.e. this builder is overwhelmingly a
// REJECTED ADMISSION TEST, not a map build, and its cost is the texels it walks BEFORE refusing.
// `g_g618Visited` is that denominator; the rej* counters name which conjunct refused
// (g603_admission_census_names_the_blocker), so a conjunct decidable without the walk can be
// hoisted out of it.
static std::atomic<uint64_t> g_g618Visited{0}, g_g618Built{0};
static std::atomic<uint64_t> g_g618RejPre{0}, g_g618RejPage{0}, g_g618RejWord{0},
    g_g618RejSx{0}, g_g618RejSy{0};
// Refusal-ring counters (the ring itself needs G326G281Key and is defined further down).
static std::atomic<uint64_t> g_g618NegHit{0}, g_g618NegMiss{0};

// ⛔ The self-test verdict must be read by the DISPATCHER, not by the kernel. A kernel that
// answered `false` on a failed self-test would be telling its caller "this view has no valid map",
// which is a BEHAVIOUR change (the T8 alias view would be refused), not a fallback. Fail-closed
// here means "run the shipped builder", so the verdict is a separate accessor.
static const G618P8Page &g618P8Page()
{
    static const G618P8Page s_page;
    return s_page;
}

enum : int
{
    kG618RejNone = 0,
    kG618RejPre = 1,   // refused before the walk (shape / source residency)
    kG618RejPage = 2,  // a sampled physical page is out of range or foreign-owned
    kG618RejWord = 3,  // the CT32 inverse has no entry for this word
    kG618RejSx = 4,    // source X outside the source framebuffer width
    kG618RejSy = 5     // source Y outside the source target's RESIDENT COVERAGE rows
};

// `rej`/`visited` are census out-params only; they cost nothing in the loop because the visited
// count at a refusal is just the loop indices. Behaviour is identical with or without them.
static bool g618BuildT8MapFast(const G144Entry &e, int srcTi, int texW, int texH,
                               std::vector<uint32_t> &out, int &rej, uint64_t &visited)
{
    rej = kG618RejNone;
    visited = 0u;
    static const G281Ct32Inverse s_inv;
    const G618P8Page &s_page = g618P8Page();
    if (!s_inv.ok || !s_page.ok || srcTi < 0 || srcTi >= static_cast<int>(kG248TargetCount) ||
        texW <= 0 || texH <= 0 || texW > g599T8MaxW() || texH > 512)
    {
        rej = kG618RejPre;
        return false;
    }
    const G261Res &src = g_g261Res[srcTi];
    if (!src.dirty || !src.range.valid || src.fbw == 0u || src.fbW <= 0 || src.fbW > 512)
    {
        rej = kG618RejPre;
        return false;
    }
    const uint32_t srcBase = framePageBaseToBlock(kG261Fbp[srcTi]);
    const uint32_t pageBwT8 = std::max<uint32_t>(1u, e.ctx.tex0.tbw >> 1u);
    out.resize(static_cast<size_t>(texW) * texH);

    constexpr uint32_t kG599PageMemo = 256u;
    uint8_t pageOk[kG599PageMemo] = {};
    int32_t pageColTerm[kG599PageMemo] = {};
    int32_t pageRowTerm[kG599PageMemo] = {};
    // A page ABOVE the memo window is never cached by the shipped loop, so it re-ran the whole
    // 14-target walk AND the divide on every one of its texels. One entry is enough: the loop walks
    // a row at a time and a PSMT8 page is 128 texels wide.
    uint32_t farBlock = 0xFFFFFFFFu;
    int32_t farCol = 0, farRow = 0;

    const uint32_t bpBase = e.ctx.tex0.tbp0 * 256u;
    const int fbW = src.fbW;
    const int covLo = src.covLo, covHi = src.covHi;
    const uint32_t fbwSrc = src.fbw;
    const int16_t *const invX = s_inv.x.data();
    const int16_t *const invY = s_inv.y.data();
    const uint16_t *const perm = s_page.perm.data();
    const size_t invN = s_inv.x.size();
    uint32_t *const dst = out.data();

    for (int vv = 0; vv < texH; ++vv)
    {
        const uint32_t yPageTerm = (static_cast<uint32_t>(vv) >> 6u) * pageBwT8;
        const uint16_t *const permRow = perm + (static_cast<uint32_t>(vv) & 63u) * 128u;
        uint32_t *const dstRow = dst + static_cast<size_t>(vv) * texW;
        for (int uu = 0; uu < texW; ++uu)
        {
            const uint32_t off = bpBase +
                                 ((yPageTerm + (static_cast<uint32_t>(uu) >> 7u)) << 13u) +
                                 permRow[static_cast<uint32_t>(uu) & 127u];
            const uint32_t pageBlock = (off >> 13u) << 5u;
            const uint32_t memoIdx = pageBlock >= srcBase ? ((pageBlock - srcBase) >> 5u)
                                                          : kG599PageMemo;
            int32_t colTerm, rowTerm;
            if (memoIdx < kG599PageMemo && pageOk[memoIdx] != 0u)
            {
                colTerm = pageColTerm[memoIdx];
                rowTerm = pageRowTerm[memoIdx];
            }
            else if (memoIdx >= kG599PageMemo && pageBlock == farBlock)
            {
                colTerm = farCol;
                rowTerm = farRow;
            }
            else
            {
                const G144BlockRange page{pageBlock, pageBlock + 31u, true};
                if (page.last > 0x3FFFu || page.first < src.range.first ||
                    page.last > src.range.last || pageBlock < srcBase ||
                    ((pageBlock - srcBase) & 31u) != 0u)
                {
                    rej = kG618RejPage;
                    visited = static_cast<uint64_t>(vv) * texW + uu;
                    return false;
                }
                for (uint32_t t = 0; t < kG248TargetCount; ++t)
                {
                    if (static_cast<int>(t) == srcTi || !g_g261Res[t].dirty)
                        continue;
                    if (!g_g261Res[t].range.valid ||
                        g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock))
                    {
                        rej = kG618RejPage;
                        visited = static_cast<uint64_t>(vv) * texW + uu;
                        return false;
                    }
                }
                const uint32_t pageOff = (pageBlock - srcBase) >> 5u;
                colTerm = static_cast<int32_t>(pageOff % fbwSrc) * 64;
                rowTerm = static_cast<int32_t>(pageOff / fbwSrc) * 32;
                if (memoIdx < kG599PageMemo)
                {
                    pageOk[memoIdx] = 1u;
                    pageColTerm[memoIdx] = colTerm;
                    pageRowTerm[memoIdx] = rowTerm;
                }
                else
                {
                    farBlock = pageBlock;
                    farCol = colTerm;
                    farRow = rowTerm;
                }
            }

            const uint32_t word = (off & 8191u) >> 2u;
            if (word >= invN)
            {
                rej = kG618RejWord;
                visited = static_cast<uint64_t>(vv) * texW + uu;
                return false;
            }
            const int sx = static_cast<int>(colTerm) + invX[word];
            const int sy = static_cast<int>(rowTerm) + invY[word];
            if (sx < 0 || sx >= fbW || sy < covLo || sy > covHi || sy < 0 || sy >= 512)
            {
                rej = (sx < 0 || sx >= fbW) ? kG618RejSx : kG618RejSy;
                visited = static_cast<uint64_t>(vv) * texW + uu;
                return false;
            }
            const uint32_t glY = static_cast<uint32_t>(511 - sy);
            const uint32_t lane = off & 3u;
            dstRow[uu] = (static_cast<uint32_t>(sx) & 0xFFu) |
                         ((glY & 0xFFu) << 8u) |
                         ((((glY >> 8u) & 1u) | (lane << 1u) |
                           (((static_cast<uint32_t>(sx) >> 8u) & 1u) << 3u))
                          << 16u) |
                         0xFF000000u;
        }
    }
    visited = static_cast<uint64_t>(texW) * static_cast<uint64_t>(texH);
    return true;
}

// ⚠️ Keyed on calls + negative-cache HITS, not on calls alone: once the refusal ring works, `calls`
// stops advancing and a calls-only cadence would print nothing on the very configuration the phase
// exists to measure.
static void g618Report()
{
    static uint64_t s_last = 0u;
    const uint64_t calls = g_g618Calls.load(std::memory_order_relaxed);
    const uint64_t negHit = g_g618NegHit.load(std::memory_order_relaxed);
    const uint64_t events = calls + negHit;
    if (events < s_last + 20000u)
        return;
    s_last = events;
    const double ns = static_cast<double>(g_g618Ns.load(std::memory_order_relaxed));
    const double vis = static_cast<double>(g_g618Visited.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G618:t8map] calls=%llu built=%llu visited=%llu builtTexels=%llu "
                 "ms=%.1f usPerCall=%.2f nsPerVisited=%.2f "
                 "rej(pre=%llu page=%llu word=%llu sx=%llu sy=%llu) "
                 "neg(hit=%llu ins=%llu) checks=%llu bad=%llu decline=%llu\n",
                 static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(g_g618Built.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618Visited.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618Texels.load(std::memory_order_relaxed)),
                 ns / 1.0e6, ns / 1.0e3 / std::max<double>(1.0, static_cast<double>(calls)),
                 ns / std::max(1.0, vis),
                 static_cast<unsigned long long>(g_g618RejPre.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618RejPage.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618RejWord.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618RejSx.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618RejSy.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(negHit),
                 static_cast<unsigned long long>(g_g618NegMiss.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618Checks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618Bad.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_g618Decline.load(std::memory_order_relaxed)));
}

// Dispatcher. Keeps `g281BuildT8MapCold` authoritative for the rollback arm AND as the oracle.
static bool g618T8Map(const G144Entry &e, int srcTi, int texW, int texH,
                      std::vector<uint32_t> &out)
{
    if (!g618FastT8MapOn() || !g618P8Page().ok)
    {
        if (g618FastT8MapOn())
            g_g618Decline.fetch_add(1u, std::memory_order_relaxed);
        return g281BuildT8MapCold(e, srcTi, texW, texH, out);
    }

    const bool stat = g618T8MapStatOn();
    const auto t0 = stat ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{};
    int rej = kG618RejNone;
    uint64_t visited = 0u;
    const bool ok = g618BuildT8MapFast(e, srcTi, texW, texH, out, rej, visited);
    if (stat)
    {
        g_g618Ns.fetch_add(static_cast<uint64_t>(
                               std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0).count()),
                           std::memory_order_relaxed);
        g_g618Calls.fetch_add(1u, std::memory_order_relaxed);
        g_g618Visited.fetch_add(visited, std::memory_order_relaxed);
        if (ok)
        {
            g_g618Built.fetch_add(1u, std::memory_order_relaxed);
            g_g618Texels.fetch_add(static_cast<uint64_t>(texW) * static_cast<uint64_t>(texH),
                                   std::memory_order_relaxed);
        }
        switch (rej)
        {
        case kG618RejPre: g_g618RejPre.fetch_add(1u, std::memory_order_relaxed); break;
        case kG618RejPage: g_g618RejPage.fetch_add(1u, std::memory_order_relaxed); break;
        case kG618RejWord: g_g618RejWord.fetch_add(1u, std::memory_order_relaxed); break;
        case kG618RejSx: g_g618RejSx.fetch_add(1u, std::memory_order_relaxed); break;
        case kG618RejSy: g_g618RejSy.fetch_add(1u, std::memory_order_relaxed); break;
        default: break;
        }
        g618Report();
    }

    if (g618T8MapVerifyOn())
    {
        // ⭐ The oracle compares the WHOLE PRODUCT and the RETURN VALUE, because a builder that
        // bails early leaves a partially-written `out` its caller never reads — so "same texels" is
        // not the property; "same decision AND same texels when accepted" is.
        static std::vector<uint32_t> s_ref;
        const bool refOk = g281BuildT8MapCold(e, srcTi, texW, texH, s_ref);
        g_g618Checks.fetch_add(1u, std::memory_order_relaxed);
        if (refOk != ok || (ok && s_ref != out))
        {
            const uint64_t bad = g_g618Bad.fetch_add(1u, std::memory_order_relaxed);
            if (bad < 16u)
                std::fprintf(stderr,
                             "[G618:bad] MISMATCH tbp=0x%x tbw=%u src=%d size=%dx%d "
                             "fastOk=%d coldOk=%d fastN=%zu coldN=%zu\n",
                             e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.tbw), srcTi,
                             texW, texH, ok ? 1 : 0, refOk ? 1 : 0, out.size(), s_ref.size());
            // The shipped builder stays authoritative under the oracle.
            if (!refOk)
                return false;
            out = s_ref;
            return true;
        }
    }
    return ok;
}

struct G326G281TargetKey
{
    int32_t fbW = 0, dirtyLo = 0, dirtyHi = 0, covLo = 0, covHi = 0;
    uint32_t rangeFirst = 0u, rangeLast = 0u;
    uint8_t fbw = 0u;
    bool dirty = false, rangeValid = false;
};
struct G326G281Key
{
    uint32_t tbp0 = 0u;
    int32_t srcTi = -1, texW = 0, texH = 0;
    uint8_t tbw = 0u, psm = 0u;
    std::array<G326G281TargetKey, kG248TargetCount> targets{};
};
static G326G281Key g326G281Key(const G144Entry &e, int srcTi, int texW, int texH)
{
    G326G281Key key{};
    key.tbp0 = e.ctx.tex0.tbp0;
    key.tbw = e.ctx.tex0.tbw;
    key.psm = e.ctx.tex0.psm;
    key.srcTi = srcTi;
    key.texW = texW;
    key.texH = texH;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        key.targets[t] = G326G281TargetKey{
            r.fbW, r.dirtyLo, r.dirtyHi, r.covLo, r.covHi,
            r.range.first, r.range.last, static_cast<uint8_t>(r.fbw),
            r.dirty, r.range.valid};
    }
    return key;
}
static bool g326G281KeyEqual(const G326G281Key &a, const G326G281Key &b)
{
    if (a.tbp0 != b.tbp0 || a.srcTi != b.srcTi || a.texW != b.texW ||
        a.texH != b.texH || a.tbw != b.tbw || a.psm != b.psm)
        return false;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G326G281TargetKey &x = a.targets[t];
        const G326G281TargetKey &y = b.targets[t];
        if (x.fbW != y.fbW || x.dirtyLo != y.dirtyLo || x.dirtyHi != y.dirtyHi ||
            x.covLo != y.covLo || x.covHi != y.covHi ||
            x.rangeFirst != y.rangeFirst || x.rangeLast != y.rangeLast ||
            x.fbw != y.fbw || x.dirty != y.dirty || x.rangeValid != y.rangeValid)
            return false;
    }
    return true;
}
struct G326G281Plan
{
    G326G281Key key{};
    std::vector<uint32_t> map;
    uint64_t stamp = 0u;
};

// ===== G618: the REFUSAL ring =====
// Rollback DC2_G618_NO_NEGCACHE=1 (independent of DC2_G618_NO_FAST_T8MAP, so the two halves of this
// phase can be gated apart).
static bool g618NegOn()
{
    static const bool on = !envFlagEnabled("DC2_G618_NO_NEGCACHE");
    return on;
}

// ⚠️ Hashed FIELD-WISE, never over the struct's bytes: `G326G281Key` is assigned member-by-member,
// so its padding is indeterminate and a byte hash could differ for two keys `g326G281KeyEqual`
// calls equal — which would silently degrade to a permanent miss. Every field hashed here is a
// field that comparator reads, so equal keys always hash equal.
static uint64_t g618KeyHash(const G326G281Key &k)
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
        h = (h ^ v) * 1099511628211ull;
    };
    mix(k.tbp0);
    mix(static_cast<uint64_t>(static_cast<uint32_t>(k.srcTi)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(k.texW)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(k.texH)));
    mix(k.tbw);
    mix(k.psm);
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G326G281TargetKey &x = k.targets[t];
        mix(static_cast<uint64_t>(static_cast<uint32_t>(x.fbW)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(x.dirtyLo)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(x.dirtyHi)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(x.covLo)));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(x.covHi)));
        mix(x.rangeFirst);
        mix(x.rangeLast);
        mix(x.fbw);
        mix(static_cast<uint64_t>(x.dirty ? 1u : 0u) |
            (static_cast<uint64_t>(x.rangeValid ? 1u : 0u) << 1u));
    }
    return h;
}

struct G618NegEntry
{
    uint64_t hash = 0u;
    bool used = false;
    G326G281Key key{};
};
// Its own storage, sized well above the accept cache: a refusal must never evict a real map, and
// the population being remembered is "every view this scene refuses", not "every view it builds".
static G618NegEntry g_g618Neg[64];
static uint32_t g_g618NegCursor = 0u;

static bool g618NegFind(const G326G281Key &key, uint64_t h)
{
    for (const G618NegEntry &n : g_g618Neg)
        if (n.used && n.hash == h && g326G281KeyEqual(n.key, key))
            return true;
    return false;
}
static void g618NegInsert(const G326G281Key &key, uint64_t h)
{
    G618NegEntry &slot = g_g618Neg[g_g618NegCursor];
    g_g618NegCursor = (g_g618NegCursor + 1u) % 64u;
    slot.hash = h;
    slot.key = key;
    slot.used = true;
}

static bool g281BuildT8Map(const G144Entry &e, int srcTi, int texW, int texH,
                            std::vector<uint32_t> &out)
{
    if (!g326G281On())
        return g618T8Map(e, srcTi, texW, texH, out);

    static std::vector<G326G281Plan> s_cache;
    static uint64_t s_stamp = 0u;
    const G326G281Key key = g326G281Key(e, srcTi, texW, texH);
    G326G281Plan *hit = nullptr;
    if (!g326ForceColdOn())
        for (G326G281Plan &plan : s_cache)
            if (g326G281KeyEqual(plan.key, key))
            {
                hit = &plan;
                break;
            }

    if (hit != nullptr)
    {
        hit->stamp = ++s_stamp;
        if (g326DiagOn())
            ++g_g326.g281Hit;
        if (g326VerifyOn())
        {
            static std::vector<uint32_t> s_cold;
            const bool coldOk = g281BuildT8MapCold(e, srcTi, texW, texH, s_cold);
            if (!coldOk || s_cold != hit->map)
            {
                ++g_g326.g281Bad;
                static uint32_t s_log = 0u;
                if (s_log++ < 16u)
                    std::fprintf(stderr,
                                 "[G326:bad] product=g281 tbp=0x%x src=%d size=%dx%d "
                                 "cold=%d cached=%zu rebuilt=%zu\n",
                                 e.ctx.tex0.tbp0, srcTi, texW, texH, coldOk ? 1 : 0,
                                 hit->map.size(), s_cold.size());
                if (!coldOk)
                    return false;
                hit->map = s_cold; // verifier repairs before the product is consumed
            }
        }
        out = hit->map;
        return true;
    }

    if (g326DiagOn())
        ++g_g326.g281Miss;

    // ===== G618: THE REFUSAL IS THE POPULATION, AND IT WAS THE ONLY THING NOT CACHED =====
    //
    // `[G618:t8map]` on `dungeon1`: **660,000 calls, 7 built, 659,993 refused — 100% of them on the
    // `sy` conjunct** (the source row falls outside the source target's resident coverage window),
    // after walking **1.96 BILLION texels** to find out: 2,972 texels and 11.62 us per call, 7.67 s
    // of GS-worker CPU, and essentially every one of those calls lands inside n=241..1081 — the
    // window where this route runs at 60 ms/f instead of 28.7 and which is why the route was
    // recorded at all.
    //
    // ⭐ The plan cache above is empty NOT because `G326G281Key` churns but because a refusal has no
    // plan to store, so the identical refused view re-walks its texels ~802 times per frame forever.
    // Caching the refusal rests on exactly the premise the ACCEPT cache already ships — that this
    // key captures every input the builder reads — so it introduces no new assumption. It gets its
    // own ring so a refusal can never evict a real map, and `DC2_G618_VERIFY=1` re-runs the shipped
    // builder on every negative hit and reports any disagreement.
    if (g618NegOn())
    {
        const uint64_t h = g618KeyHash(key);
        if (g618NegFind(key, h))
        {
            if (g618T8MapStatOn())
            {
                g_g618NegHit.fetch_add(1u, std::memory_order_relaxed);
                g618Report();
            }
            if (g618T8MapVerifyOn())
            {
                static std::vector<uint32_t> s_negRef;
                g_g618Checks.fetch_add(1u, std::memory_order_relaxed);
                if (g281BuildT8MapCold(e, srcTi, texW, texH, s_negRef))
                {
                    const uint64_t bad = g_g618Bad.fetch_add(1u, std::memory_order_relaxed);
                    if (bad < 16u)
                        std::fprintf(stderr,
                                     "[G618:bad] NEGCACHE tbp=0x%x tbw=%u src=%d size=%dx%d "
                                     "cached=refuse cold=ACCEPT\n",
                                     e.ctx.tex0.tbp0, static_cast<unsigned>(e.ctx.tex0.tbw),
                                     srcTi, texW, texH);
                    out = s_negRef; // the shipped builder stays authoritative under the oracle
                    return true;
                }
            }
            return false;
        }
        if (!g618T8Map(e, srcTi, texW, texH, out))
        {
            g618NegInsert(key, h);
            if (g618T8MapStatOn())
                g_g618NegMiss.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    }
    else if (!g618T8Map(e, srcTi, texW, texH, out))
        return false;
    if (g326ForceColdOn())
        return true;
    try
    {
        G326G281Plan plan{key, out, ++s_stamp};
        if (s_cache.size() < 16u)
            s_cache.push_back(std::move(plan));
        else
        {
            auto oldest = std::min_element(
                s_cache.begin(), s_cache.end(),
                [](const G326G281Plan &a, const G326G281Plan &b) {
                    return a.stamp < b.stamp;
                });
            *oldest = std::move(plan);
        }
    }
    catch (...)
    {
        // The exact cold product is already in `out`; cache allocation is never required.
    }
    return true;
}

static bool g310T8Base(uint32_t tbp)
{
    return tbp == 0x3420u || tbp == 0x3460u ||
           tbp == 0x34a0u || tbp == 0x34e0u;
}

// G570: the combat/jump route repeatedly reads one 64x64 PSMT8 tile at 0x2760 while the
// containing 0x139/fbw=8 CT32 atlas is GPU-resident. Legacy handling materializes all 416/448
// atlas rows for this tiny read; that publication also moves physical page generations beneath
// the newly resident 0x13d target and forces its fail-loud residency drop. Reuse G281's exact
// byte-address map + CLUT decoder for this measured tuple. The map builder admits the view only
// when every sampled physical page has exactly one dirty owner and the CLUT remains guest-owned.
// Kill: DC2_G570_NO_T8_ALIAS_VIEW=1 (DC2_G570_T8_ALIAS_VIEW=0 is equivalent).
static bool g570T8AliasViewOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G570_NO_T8_ALIAS_VIEW") ||
            envFlagDisabled("DC2_G570_T8_ALIAS_VIEW"))
            return false;
        return true;
    }();
    return s_on && g261WaveOn();
}

// G310 current-binary census: the remaining dominant publications are three 128x128 PSMT8
// triangle-family reads from high pages of the dirty 0x139/fbw=8 atlas back into that atlas.
// Snapshot those reads through G281's exact decoder. The separate texture avoids an illegal GL
// feedback loop; all page/CLUT ownership checks stay fail-closed.
static int g310TryT8Entry(uint32_t batchFbp, const G144Entry &e,
                          const G178EntryState &st)
{
    g_g310T8Candidates.fetch_add(1u, std::memory_order_relaxed);
    const bool triType =
        e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP;
    if (batchFbp != 0x139u || !e.prim.tme || e.prim.fst ||
        !triType || !st.bilinear ||
        e.ctx.tex0.psm != GS_PSM_T8 || !g310T8Base(e.ctx.tex0.tbp0) ||
        e.ctx.tex0.tbw != 2u || st.texW != 128 || st.texH != 128)
    {
        g_g310T8RejShape.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const int srcTi = g248TargetIndex(0x139u);
    if (srcTi < 0 || !g_g261Res[srcTi].dirty || g_g261Res[srcTi].fbw != 8u ||
        g_g261Res[srcTi].fbW != 512)
    {
        g_g310T8RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid)
    {
        g_g310T8RejClut.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            (!g_g261Res[t].range.valid ||
             g144RangeOverlaps(clut, g_g261Res[t].range)))
        {
            g_g310T8RejClut.fetch_add(1u, std::memory_order_relaxed);
            return -1;
        }
    static std::vector<uint32_t> s_probeMap;
    if (!g281BuildT8Map(e, srcTi, st.texW, st.texH, s_probeMap))
    {
        g_g310T8RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    return srcTi;
}

// Evidence-limited admission for the consumer G280 identified. This deliberately does not
// generalize to arbitrary PSMT8/CT32 aliases: shape, source layout, page ownership, and CLUT
// independence must all match the current-binary census or the legacy materialize path wins.
static int g281TryEntry(uint32_t batchFbp, const G144Entry &e,
                        const G178EntryState &st)
{
    // The retained G262 census identifies both as FST bilinear SPRITE consumers. Keep the
    // admission evidence-limited: G281's original 0x143<-0x13c 128x128 tuple, plus G570's
    // 0x13c<-0x139 64x64 combat tuple. Everything else takes the materialize fallback.
    const bool g281Tuple =
        batchFbp == 0x143u && e.ctx.tex0.tbp0 == 0x2820u &&
        e.ctx.tex0.tbw == 2u && st.texW == 128 && st.texH == 128;
    const bool g570Tuple =
        batchFbp == 0x13cu && e.ctx.tex0.tbp0 == 0x2760u &&
        e.ctx.tex0.tbw == 1u && st.texW == 64 && st.texH == 64;
    if ((!g281Tuple && !g570Tuple) || !e.prim.tme || !e.prim.fst ||
        e.prim.type != GS_PRIM_SPRITE || !st.bilinear ||
        e.ctx.tex0.psm != GS_PSM_T8)
    {
        g_g281RejShape.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    g_g281Candidates.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t srcFbp = g570Tuple ? 0x139u : 0x13cu;
    const uint32_t srcFbw = g570Tuple ? 8u : 2u;
    const int srcFbW = g570Tuple ? 512 : 128;
    const int srcTi = g248TargetIndex(srcFbp);
    if (srcTi < 0 || !g_g261Res[srcTi].dirty || g_g261Res[srcTi].fbw != srcFbw ||
        g_g261Res[srcTi].fbW != srcFbW)
    {
        g_g281RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid)
    {
        g_g281RejClut.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            (!g_g261Res[t].range.valid ||
             g144RangeOverlaps(clut, g_g261Res[t].range)))
        {
            g_g281RejClut.fetch_add(1u, std::memory_order_relaxed);
            return -1;
        }
    static std::vector<uint32_t> s_probeMap;
    if (!g281BuildT8Map(e, srcTi, st.texW, st.texH, s_probeMap))
    {
        g_g281RejPage.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    return srcTi;
}

// ===== G598: a PSMT8 read of ANY dirty resident target's window, not four hardcoded bases =====
//
// ⛔ THE BUG THIS FIXES. A PSMT8 texture read whose base lies inside a GPU-resident target's VRAM
// window must be served by G281's decoded view, because the target's live pixels are in the FBO and
// guest VRAM holds only what the last materialize left there. Whether that read is served was
// decided by THREE HARDCODED TUPLE LISTS:
//
//   g310TryT8Entry   batchFbp==0x139 && tbp0 in {0x3420,0x3460,0x34a0,0x34e0} && tbw==2 && 128x128
//   g281TryEntry     batchFbp==0x143 && tbp0==0x2820 && tbw==2 && 128x128        (SPRITE/FST)
//                 or batchFbp==0x13c && tbp0==0x2760 && tbw==1 && 64x64          (SPRITE/FST)
//
// The MAP-15 cutscene sky is a FOURTH measured tuple that is in none of them: a TRIANGLE-family
// read of **`tbp0=0x2720`** — which is `framePageBaseToBlock(0x139)`, target 0x139's OWN BASE —
// from a batch whose destination is **`fbp=0x13b`**. G598 proved it (identical guest packet stream
// in both arms, `fbp=0x13b tbp=0x2720 psm=19` = 14,352 vertices with and without residency; the
// sky breaks only when 0x139 is resident). It fell through every admission and took the legacy
// VRAM decode, which read the stale mirror — a whole stretched stone texture where the sky belongs.
//
// ⭐ THE LISTS ARE NOT THE SAFETY PROPERTY. `g281BuildT8MapCold` already proves, per sampled texel,
// that the page is inside the source's range, page-aligned to its base, in `covLo..covHi`, and
// owned by NO OTHER dirty target — and `g281TryEntry`/`g310TryT8Entry` separately prove the CLUT is
// still guest-owned. The tuple lists are bring-up narrowing on top of a proof that is already
// general and already fail-closed. Adding a fourth literal would be a per-screen fix for a defect
// whose whole shape is "the list has no entry for me"; this resolves the SOURCE instead and lets
// the existing proof decide, so the next texture that lands in a resident window is served too.
//
//   DC2_G598_T8ANY=1     arm (default OFF during bring-up; flip to default-ON at promotion)
//   DC2_G598_NO_T8ANY=1  rollback once promoted
//   DC2_G598_T8WHY=1     census: population + per-conjunct rejections + the observed tuples
static bool g598T8AnyOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G598_NO_T8ANY") || envFlagDisabled("DC2_G598_T8ANY"))
            return false;
        return envFlagEnabled("DC2_G598_T8ANY");
    }();
    return s_on && g261WaveOn();
}

#if defined(PS2X_G684_HOT_DIAG)
static bool g598T8WhyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G598_T8WHY");
    return s_on;
}
#else
static constexpr bool g598T8WhyOn() { return false; }
#endif

static std::atomic<uint64_t> g_g598T8Seen{0}, g_g598T8Src{0}, g_g598T8Admit{0},
    g_g598T8RejShape{0}, g_g598T8RejNoSrc{0}, g_g598T8RejClut{0}, g_g598T8RejMap{0},
    g_g598T8Ambiguous{0};

// Which dirty resident target's window does this PSMT8 texture base live in?
// Deterministic by construction: an EXACT base match wins (the sky's `tbp0 == 0x2720` is precisely
// `framePageBaseToBlock(0x139)`), otherwise containment must be UNIQUE. Two dirty targets claiming
// the same base is exactly the multi-owner case G304 has refuted six skip levers on, so it fails
// closed to the materialize path rather than guessing an owner.
static int g598FindT8Source(uint32_t tbp0)
{
    int exact = -1, contain = -1, containN = 0;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty || !r.range.valid || r.fbw == 0u || r.fbW <= 0)
            continue;
        const uint32_t base = framePageBaseToBlock(kG261Fbp[t]);
        if (tbp0 == base)
        {
            if (exact >= 0)
                return -1; // two dirty targets share a base: fail closed
            exact = static_cast<int>(t);
            continue;
        }
        if (tbp0 > base && tbp0 <= r.range.last && ((tbp0 - base) & 31u) == 0u)
        {
            contain = static_cast<int>(t);
            ++containN;
        }
    }
    if (exact >= 0)
        return exact;
    if (containN > 1)
    {
        g_g598T8Ambiguous.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    return contain;
}

// Triangle-family twin of g310TryT8Entry with the destination/base whitelist replaced by the source
// lookup above. Every remaining conjunct is one the shipped code already applies, in the same order,
// so a rejection here is a rejection the default path would also have made.
static int g598TryT8EntryAny(const G144Entry &e, const G178EntryState &st)
{
    // ⭐ Every reject counter is gated on the CENSUS flag, not on the lever. The shape reject fires
    // once per entry per flush on a ~141 k-descriptor pole thread; an unconditional atomic there
    // would be a standing tax the moment this promotes to default-ON (G593's lesson, in the
    // instrument's own file rather than the hot loop's).
    const bool why = g598T8WhyOn();
    // ⭐⭐ THE MEASURED GAP IS A **PRIM SHAPE**, NOT AN ADDRESS. The MAP-15 sky is
    // `fbp=0x13b tbp=0x2720 psm=T8 TRISTRIP fst=1` (tools/g598_tbp.py). Every shipped admission is
    // one of two disjoint shapes and it matches NEITHER:
    //     g310TryT8Entry : triangle family AND **!fst**   (STQ triangles)
    //     g281TryEntry   : **fst** AND prim == SPRITE     (UV sprites)
    // An FST TRIANGLE falls between them. FST is a TEXTURE-COORDINATE space (UV vs STQ/perspective)
    // and cannot bear on whether a TEXEL SNAPSHOT is correct: g281BuildT8Map derives its byte map
    // from tex0.tbp0/tbw and the TEX0 TW/TH dimensions and never reads a coordinate. So the
    // conjunct is bring-up narrowing from the tuples that happened to be measured, and dropping it
    // is not a relaxation of any safety property — the page-ownership proof below is untouched.
    // TRIFAN is admitted for the same reason: it is the third member of the same family.
    //
    // ⭐⭐⭐ …AND THE PRIM TYPE IS NOT A SAFETY PROPERTY EITHER. `[G598:t8why] BASEHIT` reports the
    // MAP-15 sky entry as `prim=6` (SPRITE) `fst=1` — the GIF stream's PRIM register said TRISTRIP,
    // but the entry the renderer admits is a SPRITE. It therefore never reaches the triangle
    // pre-pass at all; it reaches the SPRITE site, whose `g281TryEntry` rejects it on the
    // {0x143<-0x2820, 0x13c<-0x2760} tuple list. Both families are admitted here, and the
    // byte-map/CLUT ownership proof is what decides.
    const bool shapeOk =
        e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
        e.prim.type == GS_PRIM_TRIFAN || e.prim.type == GS_PRIM_SPRITE;
    if (st.texKey == 0u || st.skip || !e.prim.tme || !shapeOk ||
        !st.bilinear || e.ctx.tex0.psm != GS_PSM_T8 ||
        st.texW <= 0 || st.texH <= 0 || st.texW > g599T8MaxW() || st.texH > 512)
    {
        // Report a T8 entry that HAS a resident source but was refused on shape — the line that
        // names the next gap instead of leaving it as an unexplained `rej(shape=)` bucket.
        if (why && e.ctx.tex0.psm == GS_PSM_T8 &&
            g598FindT8Source(e.ctx.tex0.tbp0) >= 0)
        {
            const uint64_t n =
                g_g598T8RejShape.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(stderr,
                             "[G598:t8why] REJ-SHAPE fbp=0x%x tbp=0x%x prim=%u fst=%u tme=%u "
                             "bilin=%u %dx%d skip=%u key=%u\n",
                             e.ctx.frame.fbp, e.ctx.tex0.tbp0, e.prim.type,
                             e.prim.fst ? 1u : 0u, e.prim.tme ? 1u : 0u,
                             st.bilinear ? 1u : 0u, st.texW, st.texH,
                             st.skip ? 1u : 0u, st.texKey != 0u ? 1u : 0u);
        }
        else if (why)
            g_g598T8RejShape.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    if (why)
        g_g598T8Seen.fetch_add(1u, std::memory_order_relaxed);
    const int srcTi = g598FindT8Source(e.ctx.tex0.tbp0);
    if (srcTi < 0)
    {
        if (why)
            g_g598T8RejNoSrc.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    if (why)
        g_g598T8Src.fetch_add(1u, std::memory_order_relaxed);
    // CLUT must still be guest-authoritative — identical test to g310TryT8Entry.
    const G144BlockRange clut = g144ClutRange(e);
    if (!clut.valid)
    {
        if (why)
            g_g598T8RejClut.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            (!g_g261Res[t].range.valid ||
             g144RangeOverlaps(clut, g_g261Res[t].range)))
        {
            if (why)
                g_g598T8RejClut.fetch_add(1u, std::memory_order_relaxed);
            return -1;
        }
    // The real safety property: exact byte map with single-owner pages, fail-closed.
    static std::vector<uint32_t> s_probeMap;
    if (!g281BuildT8Map(e, srcTi, st.texW, st.texH, s_probeMap))
    {
        if (why)
            g_g598T8RejMap.fetch_add(1u, std::memory_order_relaxed);
        return -1;
    }
    if (g598T8WhyOn())
    {
        const uint64_t n = g_g598T8Admit.load(std::memory_order_relaxed);
        if (n < 24u || (n % 4096u) == 0u)
            std::fprintf(stderr,
                         "[G598:t8why] ADMIT fbp=0x%x tbp=0x%x tbw=%u psm=%u %dx%d src=%d "
                         "srcFbp=0x%x fbw=%u cov=%d..%d\n",
                         e.ctx.frame.fbp, e.ctx.tex0.tbp0, e.ctx.tex0.tbw, e.ctx.tex0.psm,
                         st.texW, st.texH, srcTi, kG261Fbp[srcTi],
                         g_g261Res[srcTi].fbw, g_g261Res[srcTi].covLo, g_g261Res[srcTi].covHi);
    }
    g_g598T8Admit.fetch_add(1u, std::memory_order_relaxed);
    return srcTi;
}

static void g598T8Report()
{
    if (!g598T8WhyOn() && !g598T8AnyOn())
        return;
    const auto v = [](const std::atomic<uint64_t> &a) {
        return (unsigned long long)a.load(std::memory_order_relaxed);
    };
    std::fprintf(stderr,
                 "[G598:t8] shaped=%llu haveSrc=%llu admit=%llu "
                 "rej(shape=%llu noSrc=%llu clut=%llu map=%llu) ambiguousSrc=%llu\n",
                 v(g_g598T8Seen), v(g_g598T8Src), v(g_g598T8Admit),
                 v(g_g598T8RejShape), v(g_g598T8RejNoSrc), v(g_g598T8RejClut),
                 v(g_g598T8RejMap), v(g_g598T8Ambiguous));
}

static bool g280VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G280_VERIFY");
    return s_on;
}

static void g280DropOverlays(int ti, uint64_t *)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount) || g_g280Ovl[ti].empty())
        return;
    g_g280Drops.fetch_add(g_g280Ovl[ti].size(), std::memory_order_relaxed);
    g_g280Ovl[ti].clear();
}

// Overwrite one overlay page tile in the DST FBO with the current guest VRAM bytes (the page
// authority whenever the src FBO does not own it). 64x32 CT32 tile packed in GL bottom-first
// row order through the DST layout's swizzle — the exact inverse of g261Materialize's writeback.
static bool g280RestoreOverlayTile(int dstTi, const G280Ovl &o)
{
    const G261Res &r = g_g261Res[dstTi];
    uint8_t *vram = g_g144LastVram;
    const int x0 = static_cast<int>(o.dstCol) * 64;
    const int y0 = static_cast<int>(o.dstRow) * 32;
    if (vram == nullptr || g_g261VramSize == 0u || r.fbW <= 0 || r.fbw == 0u ||
        x0 + 64 > r.fbW || y0 + 32 > 512)
        return false;
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[dstTi]);
    static std::vector<uint32_t> s_px; // flush-owner thread only
    s_px.assign(64u * 32u, 0u);
    for (int gy = 0; gy < 32; ++gy)
    {
        const size_t bufRow = static_cast<size_t>(31 - gy) * 64u; // GL rows are bottom-first
        for (int gx = 0; gx < 64; ++gx)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw,
                                                        static_cast<uint32_t>(x0 + gx),
                                                        static_cast<uint32_t>(y0 + gy));
            if (off + 4u > g_g261VramSize)
                return false;
            std::memcpy(&s_px[bufRow + static_cast<size_t>(gx)], vram + off, 4);
        }
    }
    return g264_backend_write_color_rect(kG261Fbp[dstTi], r.fbW, 512, x0, 512 - 32 - y0,
                                         64, 32, s_px);
}

// The DST target is about to publish its dirty rows to VRAM. Overlay tiles are a read cache of
// pages the dst view does NOT own — re-sync them from VRAM first so the publication cannot
// write alias-page bytes whose authority is the src FBO (still dirty, publishes at its own
// edge) or VRAM itself. Returns false on any restore failure: the caller must take the loud
// residency-drop path WITHOUT a readback (same contract as a G264 mirror failure).
static bool g280PrepareMaterialize(int ti)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return true;
    auto &ovl = g_g280Ovl[ti];
    if (ovl.empty())
        return true;
    bool ok = true;
    for (const G280Ovl &o : ovl)
    {
        if (g280RestoreOverlayTile(ti, o))
            g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            ok = false;
            const uint64_t n = g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
                std::fprintf(stderr, "[G280:restore-fail] n=%llu dst=%03x page=0x%x\n",
                             static_cast<unsigned long long>(n), kG261Fbp[ti], o.pageBlock);
        }
    }
    g_g280Drops.fetch_add(ovl.size(), std::memory_order_relaxed);
    ovl.clear();
    ++g_g280ContentGen[ti]; // the restores changed the dst FBO content
    return ok;
}

// A target's FBO content changed through a wave render or a full fb upload over rows
// [rowLo..rowHi]. Bump its version (staling every overlay copied FROM it) and drop any of ITS
// OWN overlay tiles the write overlaps: the resolve reproduced the physical page state of its
// moment, so later dst draws layer on top exactly as GS command order would — the tile becomes
// ordinary dst content and publishes with the dst view from now on.
static void g280NoteFboContentChange(int ti, int rowLo, int rowHi)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    ++g_g280ContentGen[ti];
    g283NoteTargetPages(ti, rowLo, rowHi); // G283: this write is the new authority for its pages
    auto &ovl = g_g280Ovl[ti];
    for (size_t k = ovl.size(); k-- > 0;)
    {
        const int ty0 = static_cast<int>(ovl[k].dstRow) * 32;
        if (ty0 + 31 >= rowLo && ty0 <= rowHi)
        {
            g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
            ovl[k] = ovl.back();
            ovl.pop_back();
        }
    }
}

// A G264 mirror patched VRAM-authoritative upload bytes into this target's FBO rows [lo..hi].
// Overlay tiles the patch overlaps are re-synced from VRAM and dropped: VRAM is the newest
// authority for the patched pixels and the tile may now be a mix of mirror and resolve bytes.
static void g280NoteMirrorPatch(int ti, int lo, int hi)
{
    if (ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    ++g_g280ContentGen[ti];
    g283NoteTargetPages(ti, lo, hi); // G283: VRAM-authoritative mirror is the new page authority
    auto &ovl = g_g280Ovl[ti];
    for (size_t k = ovl.size(); k-- > 0;)
    {
        const int ty0 = static_cast<int>(ovl[k].dstRow) * 32;
        if (ty0 + 31 < lo || ty0 > hi)
            continue;
        if (g280RestoreOverlayTile(ti, ovl[k]))
            g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
        else
        {
            const uint64_t n = g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 16u)
                std::fprintf(stderr, "[G280:restore-fail] mirror n=%llu dst=%03x page=0x%x\n",
                             static_cast<unsigned long long>(n), kG261Fbp[ti],
                             ovl[k].pageBlock);
        }
        g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
        ovl[k] = ovl.back();
        ovl.pop_back();
    }
}

// Once per flush while overlays exist: any entry whose src target is no longer dirty (VRAM
// became the page authority when the src published) or whose src contentGen moved (the copy is
// stale) is re-synced from VRAM and dropped. A consumer the admission path handles will simply
// re-resolve fresh; a legacy-path bind in between then reads exactly what it reads today
// without G280 (VRAM-consistent bytes) — never a stale resolve.
static void g280HygienePass()
{
    for (uint32_t ti = 0; ti < kG248TargetCount; ++ti)
    {
        auto &ovl = g_g280Ovl[ti];
        if (ovl.empty())
            continue;
        for (size_t k = ovl.size(); k-- > 0;)
        {
            const G280Ovl &o = ovl[k];
            if (o.srcTi < kG248TargetCount && g_g261Res[o.srcTi].dirty &&
                o.srcGen == g_g280ContentGen[o.srcTi] &&
                (!g283AuthorityOn() || o.pageEpoch == g283PageEpoch(o.pageBlock)))
                continue; // still current (G283: physical-page authority unchanged too)
            if (g283AuthorityOn() && o.srcTi < kG248TargetCount &&
                g_g261Res[o.srcTi].dirty && o.srcGen == g_g280ContentGen[o.srcTi])
                g_g283Stale.fetch_add(1u, std::memory_order_relaxed); // peer alias wrote the page
            if (g280RestoreOverlayTile(static_cast<int>(ti), o))
                g_g280Restores.fetch_add(1u, std::memory_order_relaxed);
            else
            {
                const uint64_t n =
                    g_g280RestoreFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u)
                    std::fprintf(stderr,
                                 "[G280:restore-fail] hygiene n=%llu dst=%03x page=0x%x\n",
                                 static_cast<unsigned long long>(n), kG261Fbp[ti],
                                 o.pageBlock);
            }
            g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
            ovl[k] = ovl.back();
            ovl.pop_back();
        }
        ++g_g280ContentGen[ti];
    }
}

// Conservative inclusive texel tap bounds {u0,u1,v0,v1} for the admitted FST consumer shapes,
// proven against the DST residency exactly like the existing bind rules (NEAREST texel extents,
// G263 bilinear sprite taps, G267 bilinear triangle hull). Returns false for any other shape.
static bool g280TapBounds(const G144Entry &e, const G178EntryState &st, const G261Res &r,
                          int taps[4])
{
    const bool triType = e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
                         e.prim.type == GS_PRIM_TRIFAN;
    if (e.prim.type == GS_PRIM_SPRITE)
    {
        if (st.bilinear)
            return g263BilinearSpriteBind(e, st, r, taps);
        int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
        for (int k = 0; k < 2; ++k)
        {
            const int uu = static_cast<int>(e.v[k].u >> 4);
            const int vv = static_cast<int>(e.v[k].v >> 4);
            u0 = std::min(u0, uu); u1 = std::max(u1, uu);
            v0 = std::min(v0, vv); v1 = std::max(v1, vv);
        }
        // Exclusive right/bottom endpoints (drawSprite): max tap is u1-1 / v1-1.
        if (!(u0 >= 0 && u1 <= r.fbW && v0 >= r.covLo && v1 <= r.covHi + 1) ||
            u1 - 1 < u0 || v1 - 1 < v0)
            return false;
        taps[0] = u0; taps[1] = u1 - 1; taps[2] = v0; taps[3] = v1 - 1;
        return true;
    }
    if (triType)
    {
        if (st.bilinear)
        {
            G267TriFootprint fp;
            if (!g267BilinearTriFootprint(e, st, r, fp))
                return false;
            taps[0] = fp.tapLoU; taps[1] = fp.tapHiU;
            taps[2] = fp.tapLoV; taps[3] = fp.tapHiV;
            return true;
        }
        int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
        for (int k = 0; k < 3; ++k)
        {
            const int uu = static_cast<int>(e.v[k].u >> 4);
            const int vv = static_cast<int>(e.v[k].v >> 4);
            u0 = std::min(u0, uu); u1 = std::max(u1, uu);
            v0 = std::min(v0, vv); v1 = std::max(v1, vv);
        }
        if (!(u0 >= 0 && u1 <= r.fbW - 1 && v0 >= r.covLo && v1 <= r.covHi))
            return false;
        taps[0] = u0; taps[1] = u1; taps[2] = v0; taps[3] = v1;
        return true;
    }
    return false;
}

static uint32_t g289OwnerSourceToken(const G144Entry &e)
{
    const bool ct24 = e.ctx.tex0.psm == GS_PSM_CT24;
    if (e.ctx.tex0.tbp0 == 0x2720u)
        return ct24 ? kG289AliasCt24Token : kG289AliasCt32Token;
    if (e.ctx.tex0.tbp0 == 0x2760u)
        return ct24 ? kG289WorkCt24Token : kG289WorkCt32Token;
    return 0u;
}

static bool g289IsOwnerSourceToken(uint32_t token)
{
    return token == kG289AliasCt32Token || token == kG289AliasCt24Token ||
           token == kG289WorkCt24Token || token == kG289WorkCt32Token;
}

static bool g289TriangleHasArea(const G144Entry &e)
{
    const int64_t ax = static_cast<int64_t>(e.v[1].x) - e.v[0].x;
    const int64_t ay = static_cast<int64_t>(e.v[1].y) - e.v[0].y;
    const int64_t bx = static_cast<int64_t>(e.v[2].x) - e.v[0].x;
    const int64_t by = static_cast<int64_t>(e.v[2].y) - e.v[0].y;
    return ax * by - ay * bx != 0;
}

static bool g289OwnerTapBounds(const G144Entry &e, const G178EntryState &st, int taps[4])
{
    float loU = FLT_MAX, hiU = -FLT_MAX, loV = FLT_MAX, hiV = -FLT_MAX;
    for (int k = 0; k < 3; ++k)
    {
        const float u = static_cast<float>(e.v[k].u) / 16.0f;
        const float v = static_cast<float>(e.v[k].v) / 16.0f;
        if (!std::isfinite(u) || !std::isfinite(v))
            return false;
        loU = std::min(loU, u); hiU = std::max(hiU, u);
        loV = std::min(loV, v); hiV = std::max(hiV, v);
    }
    if (st.bilinear)
    {
        taps[0] = static_cast<int>(std::floor(loU - 0.5f - 0.001f));
        taps[1] = static_cast<int>(std::ceil (hiU - 0.5f + 0.001f));
        taps[2] = static_cast<int>(std::floor(loV - 0.5f - 0.001f));
        taps[3] = static_cast<int>(std::ceil (hiV - 0.5f + 0.001f));
    }
    else
    {
        taps[0] = static_cast<int>(std::floor(loU));
        taps[1] = static_cast<int>(std::floor(hiU));
        taps[2] = static_cast<int>(std::floor(loV));
        taps[3] = static_cast<int>(std::floor(hiV));
    }
    const bool clampU = (e.ctx.clamp & 3u) == 1u;
    const bool clampV = ((e.ctx.clamp >> 2u) & 3u) == 1u;
    if (clampU)
    {
        taps[0] = std::clamp(taps[0], 0, 511);
        taps[1] = std::clamp(taps[1], 0, 511);
    }
    if (clampV)
    {
        taps[2] = std::clamp(taps[2], 0, 511);
        taps[3] = std::clamp(taps[3], 0, 511);
    }
    return taps[0] <= taps[1] && taps[2] <= taps[3];
}

// Exact second-stage admission for the final-composition owner view. Every actual texture tap
// must name one of the 104 copied CT32 pages (0x2760..0x345f). Degenerate triangles sample
// nothing and may carry the source token without a footprint. The 0x2720 alias needs a private
// synthetic FBO view; it never mutates the real 0x139 target.
static bool g289PrepareOwnerViewSources(const std::vector<G178EntryState> &states,
                                        std::vector<uint32_t> &sources,
                                        uint64_t &binds, bool &builtAlias)
{
    sources.assign(g_g144List.size(), 0u);
    binds = 0u;
    builtAlias = false;
    const auto reject = [&](const char *stage, size_t i, const G144Entry *e,
                            const int *taps, uint64_t page) {
        if (g289StatOn())
        {
            static std::atomic<uint32_t> s_logs{0};
            const uint32_t n = s_logs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 16u)
                std::fprintf(
                    stderr,
                    "[G289:view-detail] n=%u stage=%s entry=%zu/%zu "
                    "tex=%x/%u/%x uv=(%u,%u)(%u,%u)(%u,%u) "
                    "taps=%d..%d,%d..%d page=%llx\n",
                    n + 1u, stage, i, g_g144List.size(),
                    e ? e->ctx.tex0.tbp0 : 0u,
                    e ? static_cast<unsigned>(e->ctx.tex0.tbw) : 0u,
                    e ? e->ctx.tex0.psm : 0u,
                    e ? static_cast<unsigned>(e->v[0].u) : 0u,
                    e ? static_cast<unsigned>(e->v[0].v) : 0u,
                    e ? static_cast<unsigned>(e->v[1].u) : 0u,
                    e ? static_cast<unsigned>(e->v[1].v) : 0u,
                    e ? static_cast<unsigned>(e->v[2].u) : 0u,
                    e ? static_cast<unsigned>(e->v[2].v) : 0u,
                    taps ? taps[0] : 0, taps ? taps[1] : 0,
                    taps ? taps[2] : 0, taps ? taps[3] : 0,
                    static_cast<unsigned long long>(page));
        }
        return false;
    };
    bool needAlias = false;
    const G144BlockRange owner = g289OwnerRange();
    if (!owner.valid)
        return reject("owner", 0u, nullptr, nullptr, 0u);

    for (size_t i = 0u; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        const G178EntryState &st = states[i];
        if (st.skip || !e.prim.tme)
            continue;
        const G144BlockRange tex = g144TextureRange(e);
        if (!tex.valid || !g144RangeOverlaps(owner, tex))
            continue;
        if (!g289OwnerAliasTuple(e))
            return reject("tuple", i, &e, nullptr, 0u);
        const uint32_t token = g289OwnerSourceToken(e);
        if (token == 0u)
            return reject("token", i, &e, nullptr, 0u);
        if (g289TriangleHasArea(e))
        {
            int taps[4];
            if (!g289OwnerTapBounds(e, st, taps))
                return reject("footprint", i, &e, taps, 0u);
            bool xPages[8] = {}, yPages[16] = {};
            const auto markPages = [](int lo, int hi, int pageSize, int pageCount,
                                      bool clamp, bool *pages) {
                if (clamp)
                {
                    lo = std::clamp(lo, 0, 511);
                    hi = std::clamp(hi, 0, 511);
                }
                const int span = hi - lo + 1;
                if (!clamp && span >= 512)
                {
                    for (int p = 0; p < pageCount; ++p)
                        pages[p] = true;
                    return;
                }
                for (int coord = lo; coord <= hi; ++coord)
                {
                    int wrapped = coord;
                    if (!clamp)
                    {
                        wrapped %= 512;
                        if (wrapped < 0)
                            wrapped += 512;
                    }
                    pages[wrapped / pageSize] = true;
                }
            };
            markPages(taps[0], taps[1], 64, 8, st.wrapU == 1u, xPages);
            markPages(taps[2], taps[3], 32, 16, st.wrapV == 1u, yPages);
            for (uint32_t yp = 0u; yp < 16u; ++yp)
                for (uint32_t xp = 0u; xp < 8u; ++xp)
                {
                    if (!yPages[yp] || !xPages[xp])
                        continue;
                    const uint64_t page = static_cast<uint64_t>(e.ctx.tex0.tbp0) +
                        (static_cast<uint64_t>(yp) * 8u + xp) * 32u;
                    if (page < owner.first || page + 31u > owner.last)
                        return reject("page", i, &e, taps, page);
                }
        }
        sources[i] = token;
        needAlias |= e.ctx.tex0.tbp0 == 0x2720u;
        ++binds;
    }
    if (binds == 0u)
        return reject("empty", 0u, nullptr, nullptr, 0u);
    if (needAlias)
    {
        if (!g289_backend_build_work_alias_view())
            return reject("backend", 0u, nullptr, nullptr, 0u);
        builtAlias = true;
    }
    return true;
}

static bool g309EntryShape(uint32_t batchFbp, const G144Entry &e,
                           const G178EntryState &st)
{
    return g265DisplayTarget(batchFbp) && !st.skip && e.prim.tme && e.prim.fst &&
           e.prim.type == GS_PRIM_TRISTRIP && st.bilinear &&
           e.ctx.tex0.psm == GS_PSM_CT32 && e.ctx.tex0.tbp0 == 0x2720u &&
           e.ctx.tex0.tbw == 8u && st.texW == 512 && st.texH == 512;
}

static bool g310EntryShape(uint32_t batchFbp, const G144Entry &e,
                           const G178EntryState &st)
{
    if (g309EntryShape(batchFbp, e, st))
        return true;
    const bool format =
        e.ctx.tex0.psm == GS_PSM_CT32 || e.ctx.tex0.psm == GS_PSM_CT24;
    const bool ct24Alpha =
        e.ctx.tex0.psm != GS_PSM_CT24 ||
        (e.ctx.tex0.tcc == 1u && e.texa.ta0 == 128u &&
         !e.texa.aem && e.texa.ta1 == 128u);
    if (g310StatOn() && e.ctx.tex0.psm == GS_PSM_CT24 &&
        g265DisplayTarget(batchFbp) && !st.skip && e.prim.tme && e.prim.fst &&
        e.prim.type == GS_PRIM_SPRITE && st.bilinear &&
        e.ctx.tex0.tbp0 == 0x2720u && e.ctx.tex0.tbw == 8u &&
        st.texW == 512 && st.texH == 512)
    {
        static std::atomic<uint32_t> s_ct24Log{0};
        if (s_ct24Log.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr,
                         "[G310:ct24-shape] dst=%03x wrap=%u%u tcc=%u "
                         "texa=%u/%u/%u alphaOk=%u\n",
                         batchFbp, static_cast<unsigned>(st.wrapU),
                         static_cast<unsigned>(st.wrapV),
                         static_cast<unsigned>(e.ctx.tex0.tcc),
                         static_cast<unsigned>(e.texa.ta0), e.texa.aem ? 1u : 0u,
                         static_cast<unsigned>(e.texa.ta1), ct24Alpha ? 1u : 0u);
    }
    return g265DisplayTarget(batchFbp) && !st.skip && e.prim.tme && e.prim.fst &&
           e.prim.type == GS_PRIM_SPRITE && st.bilinear &&
           st.wrapU == 1u && st.wrapV == 1u && format && ct24Alpha &&
           e.ctx.tex0.tbp0 == 0x2720u && e.ctx.tex0.tbw == 8u &&
           st.texW == 512 && st.texH == 512;
}

// Prepare one authority map for the whole flush. The timing path binds the live sources directly;
// `useComposite` is false under the exact verifier because that arm intentionally forces today's
// legacy materialization/draw path after comparing a transient reference surface.
static bool g309PrepareComposite(bool &useComposite)
{
    useComposite = false;
    static std::vector<uint32_t> s_pageMap;
    static std::vector<uint32_t> s_basePixels;
    static std::array<G309PageSnapshot, 128> s_snapshot;
    static std::vector<uint32_t> s_composite;
    if (!g309BuildPageMap(s_pageMap, s_snapshot))
        return false;
    if (g_g144LastVram == nullptr || g_g261VramSize < 4u * 1024u * 1024u)
    {
        g_g309FailMissing.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    // Only page-map zeroes can sample the guest fallback. Preserve a full-pitch staging vector
    // so the backend can upload contiguous clean runs directly, but decode no resident page.
    s_basePixels.resize(512u * 512u);
    for (uint32_t yp = 0u; yp < 16u; ++yp)
        for (uint32_t xp = 0u; xp < 8u; ++xp)
        {
            const size_t page = static_cast<size_t>(yp) * 8u + xp;
            if ((s_pageMap[page] & 0xFFu) != 0u)
                continue;
            for (uint32_t localY = 0u; localY < 32u; ++localY)
            {
                const uint32_t gsY = yp * 32u + localY;
                const uint32_t glY = 511u - gsY;
                for (uint32_t localX = 0u; localX < 64u; ++localX)
                {
                    const uint32_t x = xp * 64u + localX;
                    s_basePixels[static_cast<size_t>(glY) * 512u + x] =
                        GSMem::ReadCT32(g_g144LastVram, 0x2720u, 8u, x, gsY);
                }
            }
        }
    const bool verify = g309VerifyOn();
    s_composite.clear();
    if (!g309_backend_build_authoritative_composite(
            s_pageMap, s_basePixels, verify, s_composite))
    {
        g_g309FailBackend.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (!g309SnapshotCurrent(s_snapshot))
    {
        g_g309FailStale.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const uint64_t compositeN =
        g_g309Composites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g309StatOn() && (compositeN == 1u || (compositeN % 64u) == 0u))
        g309Report();
    if (verify)
    {
        g309VerifyAgainstLegacy(s_composite);
        return true;
    }
    useComposite = true;
    return true;
}

// Attempt to fully handle one display-batch consumer entry that samples a dirty RTT target:
// admit the FBO-direct bind (adding the previously CPU-only bilinear TRISTRIP shape) and
// resolve every dirty-sibling physical-page alias with GPU->GPU page-tile copies. Returns the
// producer fbp to bind, or 0 to fall back to the legacy per-target bind/materialize loop
// (today's behavior, bit-exact). Fail-closed on: unproven footprints, unknown sibling windows,
// non-admitted alias tuples, misaligned/out-of-range pages, src tiles not provably
// FBO-defined, and backend copy failure — none of which mutate any state before returning 0.
static uint32_t g280TryEntry(uint32_t batchFbp, const G144Entry &e, const G178EntryState &st)
{
    if (g248TargetIndex(batchFbp) >= 0)
        return 0u; // display consumers only; RTT-family internal consumers keep existing rules
    if (!e.prim.tme || !e.prim.fst || e.ctx.tex0.psm != GS_PSM_CT32)
        return 0u;
    int dstTi = -1;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (g_g261Res[t].dirty &&
            e.ctx.tex0.tbp0 == framePageBaseToBlock(kG261Fbp[t]) &&
            e.ctx.tex0.tbw == g_g261Res[t].fbw)
        {
            dstTi = static_cast<int>(t);
            break;
        }
    if (dstTi < 0)
        return 0u;
    const G261Res &dst = g_g261Res[dstTi];
    int taps[4];
    if (!g280TapBounds(e, st, dst, taps))
    {
        g_g280RejFoot.fetch_add(1u, std::memory_order_relaxed);
        return 0u;
    }
    const uint32_t dstBase = e.ctx.tex0.tbp0;
    const uint32_t dstBw = std::max<uint32_t>(1u, e.ctx.tex0.tbw);
    const uint32_t xp0 = static_cast<uint32_t>(std::max(taps[0], 0)) / 64u;
    const uint32_t xp1 = static_cast<uint32_t>(std::max(taps[1], 0)) / 64u;
    const uint32_t yp0 = static_cast<uint32_t>(std::max(taps[2], 0)) / 32u;
    const uint32_t yp1 = static_cast<uint32_t>(std::max(taps[3], 0)) / 32u;
    static std::vector<G280Ovl> s_newOvl; // overlay records committed on copy success
    s_newOvl.clear();
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        if (static_cast<int>(t) == dstTi || !g_g261Res[t].dirty)
            continue;
        const G261Res &src = g_g261Res[t];
        if (!src.range.valid)
        {
            g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
            return 0u; // unknown sibling window — fail closed
        }
        bool anyOverlap = false;
        for (uint32_t yp = yp0; yp <= yp1; ++yp)
            for (uint32_t xp = xp0; xp <= xp1; ++xp)
            {
                const uint64_t first64 = static_cast<uint64_t>(dstBase) +
                                         (static_cast<uint64_t>(yp) * dstBw + xp) * 32u;
                if (first64 > 0x3FFFu)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const G144BlockRange page{
                    static_cast<uint32_t>(first64),
                    static_cast<uint32_t>(std::min<uint64_t>(first64 + 31u, 0x3FFFu)), true};
                if (!g144RangeOverlaps(page, src.range))
                    continue;
                anyOverlap = true;
                // Evidence-limited tuple admission: dst is the 0x139 CT32 atlas view; src is
                // any of ITS four measured CT32 work blocks (G267 proved 0x146 page 0x2920;
                // the G280 bindrej census proved 0x13c page 0x2820 in the same strip today —
                // the mapping below is identical for every family member because CT32
                // within-page pixel order is layout-independent).
                if (kG261Fbp[static_cast<uint32_t>(dstTi)] != 0x139u)
                {
                    g_g280RejTuple.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const uint32_t pageBlock = static_cast<uint32_t>(first64);
                const uint32_t srcBaseBlk = framePageBaseToBlock(kG261Fbp[t]);
                if (pageBlock < srcBaseBlk || ((pageBlock - srcBaseBlk) & 31u) != 0u ||
                    src.fbw == 0u || src.fbW <= 0)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u;
                }
                const uint32_t srcOff = (pageBlock - srcBaseBlk) >> 5u;
                const uint32_t srcRow = srcOff / src.fbw;
                const uint32_t srcCol = srcOff % src.fbw;
                const int sy0 = static_cast<int>(srcRow) * 32;
                if (static_cast<int>(srcCol + 1u) * 64 > src.fbW || sy0 + 32 > 512 ||
                    sy0 < src.covLo || sy0 + 31 > src.covHi)
                {
                    g_g280RejPage.fetch_add(1u, std::memory_order_relaxed);
                    return 0u; // src tile not provably FBO-defined under the src view
                }
                // G294: copy this page ONLY from its current authoritative owner. A DIVERGENT
                // sibling (owner recorded and it is a different target — e.g. the dst 0x139 or a
                // peer wrote the page more recently) is skipped, and the dst keeps its existing
                // overlay/VRAM bytes for the page (fail-closed). This is the exact fix for G292's
                // multi-owner page-0x2860 mismatch, where two aliasing siblings both copied in and
                // the later copy overwrote the earlier. owner<0 (unknown) keeps the pre-G294
                // single-owner behavior (already bit-exact, G280 vfyBad=0 without a multi-owner).
                if (g294OwnerTokenOn())
                {
                    const int owner = g294PageOwner(pageBlock);
                    if (owner >= 0 && owner != static_cast<int>(t))
                    {
                        g_g294NonOwnerSkip.fetch_add(1u, std::memory_order_relaxed);
                        continue; // authoritative owner is elsewhere
                    }
                    if (owner == static_cast<int>(t))
                        g_g294OwnerSel.fetch_add(1u, std::memory_order_relaxed);
                }
                bool current = false;
                for (const G280Ovl &o : g_g280Ovl[dstTi])
                    if (o.pageBlock == pageBlock && o.srcTi == static_cast<uint8_t>(t) &&
                        o.srcGen == g_g280ContentGen[t] &&
                        (!g283AuthorityOn() || o.pageEpoch == g283PageEpoch(pageBlock)))
                    {
                        current = true;
                        break;
                    }
                if (current)
                    continue;
                G280Ovl o;
                o.pageBlock = pageBlock;
                o.srcTi = static_cast<uint8_t>(t);
                o.srcGen = g_g280ContentGen[t];
                o.pageEpoch = g283PageEpoch(pageBlock); // G283: authority epoch at resolve time
                o.dstRow = static_cast<uint16_t>(yp);
                o.dstCol = static_cast<uint16_t>(xp);
                o.srcRow = static_cast<uint16_t>(srcRow);
                o.srcCol = static_cast<uint16_t>(srcCol);
                s_newOvl.push_back(o);
            }
        if (!anyOverlap)
            g_g280SibSkips.fetch_add(1u, std::memory_order_relaxed);
    }
    bool copiesOk = true;
    if (!s_newOvl.empty())
    {
        // One batched copy job per distinct src target (a strip can alias several work blocks
        // of the same atlas — the census measured 0x13c AND 0x146 in one footprint). Overlay
        // records are committed for EVERY attempted job, including a failed one: the tiles'
        // dst FBO content is then unknown, and the committed record is exactly what forces a
        // VRAM re-sync before any dst publication or later bind (hygiene/materialize paths).
        static std::vector<int32_t> s_rects;
        auto commitOvl = [&](uint8_t srcT) {
            for (const G280Ovl &o : s_newOvl)
            {
                if (o.srcTi != srcT)
                    continue;
                // G292: this GPU copy changed the dst tile without changing guest VRAM.
                g291ClearCopiedTile(dstTi, o.dstRow, o.dstCol);
                bool replaced = false;
                for (G280Ovl &ex : g_g280Ovl[dstTi])
                    if (ex.pageBlock == o.pageBlock)
                    {
                        ex = o;
                        replaced = true;
                        break;
                    }
                if (!replaced)
                    g_g280Ovl[dstTi].push_back(o);
            }
        };
        for (uint32_t t = 0; t < kG248TargetCount; ++t)
        {
            s_rects.clear();
            for (const G280Ovl &o : s_newOvl)
            {
                if (o.srcTi != static_cast<uint8_t>(t))
                    continue;
                s_rects.push_back(static_cast<int32_t>(o.srcCol) * 64);
                s_rects.push_back(512 - 32 - static_cast<int32_t>(o.srcRow) * 32);
                s_rects.push_back(static_cast<int32_t>(o.dstCol) * 64);
                s_rects.push_back(512 - 32 - static_cast<int32_t>(o.dstRow) * 32);
                s_rects.push_back(64);
                s_rects.push_back(32);
            }
            if (s_rects.empty())
                continue;
            if (t == 0u) // G351: overlay copy sourcing the wide 0x139 FBO (FBO-medium consumer)
            {
                g351NoteCon(kG351SiteOvl, 0, static_cast<uint64_t>(s_rects.size() / 6u) * 32u);
                (void)g351FanSourceSafe();
            }
            if (!g280_backend_copy_color_rects(kG261Fbp[t],
                                               kG261Fbp[static_cast<uint32_t>(dstTi)],
                                               s_rects))
            {
                copiesOk = false;
                const uint64_t n =
                    g_g280CopyFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 16u)
                    std::fprintf(stderr,
                                 "[G280:copy-fail] n=%llu dst=%03x src=%03x rects=%zu\n",
                                 static_cast<unsigned long long>(n),
                                 kG261Fbp[static_cast<uint32_t>(dstTi)], kG261Fbp[t],
                                 s_rects.size() / 6u);
                // Stale the failed tiles so no freshness test can call them current, then
                // force an immediate VRAM re-sync of everything just committed.
                for (G280Ovl &o : s_newOvl)
                    if (o.srcTi == static_cast<uint8_t>(t))
                        o.srcGen = g_g280ContentGen[t] - 1u;
            }
            commitOvl(static_cast<uint8_t>(t));
        }
        if (!copiesOk)
        {
            g280HygienePass(); // restores the failed/unknown tiles from VRAM and drops them
            return 0u;         // legacy path then reproduces today's materialize behavior
        }
        g_g280Copies.fetch_add(s_newOvl.size(), std::memory_order_relaxed);
        if (g280VerifyOn())
        {
            // Exactness check of the mapping/copy itself: both FBO tiles must be bit-identical.
            static std::vector<uint32_t> s_vs, s_vd;
            for (const G280Ovl &o : s_newOvl)
            {
                const G261Res &src = g_g261Res[o.srcTi];
                const int sGlY = 512 - 32 - static_cast<int>(o.srcRow) * 32;
                const int dGlY = 512 - 32 - static_cast<int>(o.dstRow) * 32;
                if (!g178_backend_read_color(kG261Fbp[o.srcTi], src.fbW, 512, sGlY, 32, s_vs) ||
                    !g178_backend_read_color(kG261Fbp[static_cast<uint32_t>(dstTi)], dst.fbW,
                                             512, dGlY, 32, s_vd))
                    continue;
                uint64_t bad = 0;
                for (int ry = 0; ry < 32; ++ry)
                    for (int rx = 0; rx < 64; ++rx)
                        if (s_vs[static_cast<size_t>(ry) * src.fbW +
                                 static_cast<size_t>(o.srcCol) * 64u + rx] !=
                            s_vd[static_cast<size_t>(ry) * dst.fbW +
                                 static_cast<size_t>(o.dstCol) * 64u + rx])
                            ++bad;
                if (bad != 0u)
                {
                    const uint64_t tot =
                        g_g280VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
                    std::fprintf(stderr,
                                 "[G280:vfy] MISMATCH page=0x%x bad=%llu tot=%llu "
                                 "src=%03x(%u,%u) dst=%03x(%u,%u)\n",
                                 o.pageBlock, static_cast<unsigned long long>(bad),
                                 static_cast<unsigned long long>(tot), kG261Fbp[o.srcTi],
                                 o.srcCol, o.srcRow,
                                 kG261Fbp[static_cast<uint32_t>(dstTi)], o.dstCol, o.dstRow);
                }
            }
        }
    }
    g_g280Binds.fetch_add(1u, std::memory_order_relaxed);
    return kG261Fbp[static_cast<uint32_t>(dstTi)];
}

static bool g282EntryBounds(const G144Entry &e, const G178EntryState &st,
                            int &x0, int &y0, int &x1, int &y1)
{
    const int ofx = e.ctx.xyoffset.ofx >> 4;
    const int ofy = e.ctx.xyoffset.ofy >> 4;
    if (e.prim.type == GS_PRIM_SPRITE)
    {
        int ax = static_cast<int>(e.v[0].x) - ofx;
        int ay = static_cast<int>(e.v[0].y) - ofy;
        int bx = static_cast<int>(e.v[1].x) - ofx;
        int by = static_cast<int>(e.v[1].y) - ofy;
        if (ax > bx) std::swap(ax, bx);
        if (ay > by) std::swap(ay, by);
        x0 = ax;
        y0 = ay;
        x1 = ax + std::max(1, bx - ax) - 1;
        y1 = ay + std::max(1, by - ay) - 1;
    }
    else if (e.prim.type == GS_PRIM_TRIANGLE ||
             e.prim.type == GS_PRIM_TRISTRIP ||
             e.prim.type == GS_PRIM_TRIFAN)
    {
        float minX = e.v[0].x, maxX = e.v[0].x;
        float minY = e.v[0].y, maxY = e.v[0].y;
        for (int k = 1; k < 3; ++k)
        {
            minX = std::min(minX, e.v[k].x);
            maxX = std::max(maxX, e.v[k].x);
            minY = std::min(minY, e.v[k].y);
            maxY = std::max(maxY, e.v[k].y);
        }
        x0 = static_cast<int>(std::floor(minX)) - ofx;
        y0 = static_cast<int>(std::floor(minY)) - ofy;
        x1 = static_cast<int>(std::ceil(maxX)) - ofx;
        y1 = static_cast<int>(std::ceil(maxY)) - ofy;
    }
    else
        return false;
    x0 = std::max(x0, static_cast<int>(st.scX0));
    y0 = std::max(y0, static_cast<int>(st.scY0));
    x1 = std::min(x1, static_cast<int>(st.scX1));
    y1 = std::min(y1, static_cast<int>(st.scY1));
    return x1 >= x0 && y1 >= y0;
}

// Structural admission, not a frame-count fingerprint: the first primitive must completely
// replace the thirteen measured CT32 page rows, and every later draw must stay inside that clear.
// Thus neither old color aliases nor untouched pixels are inputs to the new page contents.
static bool g282PrepareFanout(const std::vector<G178EntryState> &states,
                              bool guestDepth, bool guestDepthWrites,
                              uint32_t guestZbpPage, uint32_t guestZpsm,
                              std::vector<G282Fanout> &out,
                              bool buildOwnerPlan = true)
{
    auto logReject = [&](const char *why, size_t i, int x0, int y0, int x1, int y1) {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (g282StatOn() && n <= 16u)
            std::fprintf(stderr,
                         "[G282:admit-rej] n=%llu why=%s i=%zu bounds=%d,%d..%d,%d entries=%zu\n",
                         static_cast<unsigned long long>(n), why, i,
                         x0, y0, x1, y1, g_g144List.size());
    };
    out.clear();
    if (!guestDepth || guestDepthWrites || guestZbpPage != 0x0d0u ||
        guestZpsm != 1u || g_g144List.size() < 2u ||
        states.size() != g_g144List.size())
    {
        logReject("family", 0u, 0, 0, -1, -1);
        return false;
    }
    const G144Entry &clear = g_g144List.front();
    const G178EntryState &clearSt = states.front();
    int clearX0 = 0, clearY0 = 0, clearX1 = -1, clearY1 = -1;
    if (clear.prim.type != GS_PRIM_SPRITE || clear.prim.tme || clear.prim.abe ||
        clear.ctx.frame.fbmsk != 0u || clearSt.guestDepth || clearSt.skip ||
        clear.v[0].r != 0 || clear.v[0].g != 0 ||
        clear.v[0].b != 0 || clear.v[0].a != 0 ||
        clear.v[1].r != 0 || clear.v[1].g != 0 ||
        clear.v[1].b != 0 || clear.v[1].a != 0 ||
        !g282EntryBounds(clear, clearSt, clearX0, clearY0, clearX1, clearY1) ||
        clearX0 != 0 || clearY0 != 0 || clearX1 != 511 || clearY1 != 415)
    {
        logReject("clear", 0u, clearX0, clearY0, clearX1, clearY1);
        return false;
    }
    for (size_t i = 1; i < g_g144List.size(); ++i)
    {
        const G144Entry &e = g_g144List[i];
        const G178EntryState &st = states[i];
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        if (st.skip)
            continue;
        if (e.prim.type != GS_PRIM_TRIANGLE &&
            e.prim.type != GS_PRIM_TRISTRIP &&
            e.prim.type != GS_PRIM_TRIFAN)
        {
            logReject("prim", i, x0, y0, x1, y1);
            return false;
        }
        if (e.prim.tme || !e.prim.abe || !st.guestDepth || st.depthWrite)
        {
            logReject("draw", i, x0, y0, x1, y1);
            return false;
        }
        if (!g282EntryBounds(e, st, x0, y0, x1, y1))
            continue; // fully scissored primitive: no color write and no depth read
        if (x0 < clearX0 || x1 > clearX1 || y0 < 32 || y1 > 447)
        {
            logReject("bounds", i, x0, y0, x1, y1);
            return false;
        }
    }

    const int srcTi = g248TargetIndex(0x139u);
    if (srcTi < 0)
        return false;
    // A row-scoped source write will drop its overlay records. Fail closed if one exists in the
    // affected rows but outside the fully-overwritten page rectangle.
    for (const G280Ovl &o : g_g280Ovl[srcTi])
        if (static_cast<int>(o.dstRow) * 32 <= clearY1 &&
            static_cast<int>(o.dstRow) * 32 + 31 >= clearY0 &&
            (static_cast<int>(o.dstCol) * 64 < clearX0 ||
             static_cast<int>(o.dstCol) * 64 + 63 > clearX1))
        {
            g_g282RejOwner.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
    if (!buildOwnerPlan)
        return true;

    // G283: with the physical-page authority epoch, more than one dirty sibling may own a page.
    // Fan the completed 0x139 batch bytes to EVERY dirty owner (they are all byte-identical after
    // the copy) and let the epoch gate invalidate whichever alias is next written independently.
    // Authority OFF keeps G282's proven single-owner fail-close (bit-exact default behavior).
    const bool authority = g283AuthorityOn();
    const uint32_t srcBase = framePageBaseToBlock(0x139u);
    for (uint32_t py = 0; py < 13u; ++py)
        for (uint32_t px = 0; px < 8u; ++px)
        {
            const uint32_t pageBlock = srcBase + (py * 8u + px) * 32u;
            int owners = 0;
            for (uint32_t t = 0; t < kG248TargetCount; ++t)
            {
                if (static_cast<int>(t) == srcTi)
                    continue;
                uint32_t row = 0u, col = 0u;
                if (!g280DirtyTargetOwnsPage(static_cast<int>(t), pageBlock, &row, &col))
                    continue;
                if (owners >= 1 && !authority)
                {
                    // Multiple dirty sibling publishers for one physical page need the G283
                    // authority mechanism; without it, admit only the measured single owner.
                    out.clear();
                    g_g282RejOwner.fetch_add(1u, std::memory_order_relaxed);
                    return false;
                }
                if (owners >= 1)
                    g_g283MultiOwner.fetch_add(1u, std::memory_order_relaxed);
                ++owners;
                G282Fanout f;
                f.pageBlock = pageBlock;
                f.dstTi = static_cast<uint8_t>(t);
                f.srcRow = static_cast<uint16_t>(py);
                f.srcCol = static_cast<uint16_t>(px);
                f.dstRow = static_cast<uint16_t>(row);
                f.dstCol = static_cast<uint16_t>(col);
                out.push_back(f);
            }
        }
    return true;
}

static bool g282ExecuteFanout(const std::vector<G282Fanout> &plan)
{
    static std::vector<int32_t> s_rects;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        s_rects.clear();
        for (const G282Fanout &f : plan)
        {
            if (f.dstTi != static_cast<uint8_t>(t))
                continue;
            s_rects.push_back(static_cast<int32_t>(f.srcCol) * 64);
            s_rects.push_back(512 - 32 - static_cast<int32_t>(f.srcRow) * 32);
            s_rects.push_back(static_cast<int32_t>(f.dstCol) * 64);
            s_rects.push_back(512 - 32 - static_cast<int32_t>(f.dstRow) * 32);
            s_rects.push_back(64);
            s_rects.push_back(32);
        }
        if (s_rects.empty())
            continue;
        // G351: FBO-medium consumer — the fan-out reads the WIDE 0x139 FBO directly. If the
        // sprite band has been routed to the sibling, the fbp-keyed FBO is missing rows 416..447
        // (fail-closed detector; zero on MAP-0 — fan consumers are absent there).
        g351NoteCon(kG351SiteFan, 0, static_cast<uint64_t>(s_rects.size() / 6u) * 32u);
        (void)g351FanSourceSafe();
        if (!g280_backend_copy_color_rects(0x139u, kG261Fbp[t], s_rects))
        {
            const uint64_t n =
                g_g282FanoutFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::fprintf(stderr,
                         "[G282:fanout-fail] n=%llu dst=%03x rects=%zu\n",
                         static_cast<unsigned long long>(n), kG261Fbp[t],
                         s_rects.size() / 6u);
            return false;
        }
    }
    bool verifyOk = true;
    if (g282VerifyOn())
    {
        static std::vector<uint32_t> s_src, s_dst;
        for (const G282Fanout &f : plan)
        {
            const G261Res &dst = g_g261Res[f.dstTi];
            const int srcGlY = 512 - 32 - static_cast<int>(f.srcRow) * 32;
            const int dstGlY = 512 - 32 - static_cast<int>(f.dstRow) * 32;
            if (!g178_backend_read_color(0x139u, 512, 512, srcGlY, 32, s_src) ||
                !g178_backend_read_color(kG261Fbp[f.dstTi], dst.fbW, 512,
                                         dstGlY, 32, s_dst))
                continue;
            uint64_t bad = 0u;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x)
                    if (s_src[static_cast<size_t>(y) * 512u +
                              static_cast<size_t>(f.srcCol) * 64u + x] !=
                        s_dst[static_cast<size_t>(y) * dst.fbW +
                              static_cast<size_t>(f.dstCol) * 64u + x])
                        ++bad;
            if (bad != 0u)
            {
                verifyOk = false;
                const uint64_t tot =
                    g_g282VerifyBad.fetch_add(bad, std::memory_order_relaxed) + bad;
                std::fprintf(stderr,
                             "[G282:vfy] page=0x%x dst=%03x bad=%llu tot=%llu\n",
                             f.pageBlock, kG261Fbp[f.dstTi],
                             static_cast<unsigned long long>(bad),
                             static_cast<unsigned long long>(tot));
            }
        }
    }
    return verifyOk;
}

static void g282CommitFanout(const std::vector<G282Fanout> &plan)
{
    bool touched[kG248TargetCount] = {};
    for (const G282Fanout &f : plan)
    {
        const int ti = static_cast<int>(f.dstTi);
        G261Res &r = g_g261Res[ti];
        const int y0 = static_cast<int>(f.dstRow) * 32;
        r.dirty = true;
        r.dirtyLo = std::min(r.dirtyLo, y0);
        r.dirtyHi = std::max(r.dirtyHi, y0 + 31);
        r.covLo = std::min(r.covLo, y0);
        r.covHi = std::max(r.covHi, y0 + 31);
        // G292: fan-out copied the new 0x139 bytes into this sibling FBO tile; any prior
        // upload-re-coverage proof for the destination tile is now stale.
        g291ClearCopiedTile(ti, f.dstRow, f.dstCol);
        auto &ovl = g_g280Ovl[ti];
        for (size_t k = ovl.size(); k-- > 0;)
            if (ovl[k].pageBlock == f.pageBlock)
            {
                g_g280Drops.fetch_add(1u, std::memory_order_relaxed);
                ovl[k] = ovl.back();
                ovl.pop_back();
            }
        touched[ti] = true;
        // G283: the completed 0x139 batch is the authoritative writer of this physical page; the
        // fresh epoch stales every alias overlay/copy so a later independent peer write is caught.
        if (g283AuthorityOn())
            g283BumpPageEpoch(f.pageBlock);
    }
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        if (touched[t])
        {
            ++g_g280ContentGen[t];
            g261UpdateWindow(static_cast<int>(t));
        }
    g_g282FanoutPages.fetch_add(plan.size(), std::memory_order_relaxed);
    g_g283FanoutPages.fetch_add(plan.size(), std::memory_order_relaxed);
    if (g283StatOn())
    {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 12u || (n % 256u) == 0u)
            std::fprintf(stderr,
                         "[G283:fanout] n=%llu pages=%zu multiOwnerCum=%llu epochBumps=%llu "
                         "vfyBad=%llu\n",
                         static_cast<unsigned long long>(n), plan.size(),
                         (unsigned long long)g_g283MultiOwner.load(std::memory_order_relaxed),
                         (unsigned long long)g_g283Bumps.load(std::memory_order_relaxed),
                         (unsigned long long)g_g282VerifyBad.load(std::memory_order_relaxed));
    }
}

// After the source wave's normal row invalidation, reclassify each fanned page as a current
// read-cache overlay from its sibling owner. Both FBO tiles contain identical post-batch bytes;
// choosing the narrow sibling as authority prevents the wide 0x139 view from blocking G281.
static void g282CommitSourceOverlays(const std::vector<G282Fanout> &plan)
{
    const int srcTi = g248TargetIndex(0x139u);
    if (srcTi < 0)
        return;
    auto &ovl = g_g280Ovl[srcTi];
    for (const G282Fanout &f : plan)
    {
        G280Ovl o;
        o.pageBlock = f.pageBlock;
        o.srcTi = f.dstTi;
        o.srcGen = g_g280ContentGen[f.dstTi];
        o.pageEpoch = g283PageEpoch(f.pageBlock); // G283: current physical-page authority epoch
        o.dstRow = f.srcRow;
        o.dstCol = f.srcCol;
        o.srcRow = f.dstRow;
        o.srcCol = f.dstCol;
        bool replaced = false;
        for (G280Ovl &ex : ovl)
            if (ex.pageBlock == o.pageBlock)
            {
                ex = o;
                replaced = true;
                break;
            }
        if (!replaced)
            ovl.push_back(o);
    }
}

static void g282DropResidencyAfterFanoutFailure()
{
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
    {
        g280DropOverlays(static_cast<int>(t));
        G261Res &r = g_g261Res[t];
        r.dirty = false;
        r.dirtyLo = 512;
        r.dirtyHi = -1;
        r.covLo = 512;
        r.covHi = -1;
        r.mirPending = false;
        r.mirLo = 512;
        r.mirHi = -1;
        r.mirBits = {};
        r.g291Cov = {};
        r.g291CovFbw = 0u;
        g261UpdateWindow(static_cast<int>(t));
        G178FbSnap &snap = g_g178FbSnap[kG261Fbp[t]];
        snap.valid = false;
        snap.genSum = 0u;
        snap.rowLo = 1 << 30;
        snap.rowHi = -1;
    }
}

static void g280Report()
{
    if (!g280AliasOn() && !g280StatOn() && !g281StatOn() &&
        g_g280Binds.load(std::memory_order_relaxed) == 0u)
        return;
    size_t live = 0;
    for (uint32_t t = 0; t < kG248TargetCount; ++t)
        live += g_g280Ovl[t].size();
    std::fprintf(stderr,
                 "[G280:stat] binds=%llu copies=%llu copyFail=%llu restores=%llu "
                 "restoreFail=%llu drops=%llu sibSkip=%llu "
                 "rej(foot=%llu tuple=%llu page=%llu) vfyBad=%llu ovlLive=%zu\n",
                 (unsigned long long)g_g280Binds.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Copies.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280CopyFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Restores.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RestoreFail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280Drops.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280SibSkips.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejFoot.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejTuple.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280RejPage.load(std::memory_order_relaxed),
                 (unsigned long long)g_g280VerifyBad.load(std::memory_order_relaxed),
                 live);
    if (g281ViewOn() || g281StatOn() ||
        g_g281Views.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G281:stat] cand=%llu views=%llu buildFail=%llu "
                     "rej(shape=%llu page=%llu clut=%llu) vfyBad=%llu\n",
                     (unsigned long long)g_g281Candidates.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281Views.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281BuildFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejShape.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejPage.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281RejClut.load(std::memory_order_relaxed),
                     (unsigned long long)g_g281VerifyBad.load(std::memory_order_relaxed));
    if (g282TightRowsOn() || g282StatOn() ||
        g_g282TightBatches.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G282:stat] cand=%llu tight=%llu aliasSaved=%llu "
                     "fanout=%llu fanoutFail=%llu rej(shape=%llu owner=%llu) vfyBad=%llu\n",
                     (unsigned long long)g_g282Candidates.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282TightBatches.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282AliasSaved.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282FanoutPages.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282FanoutFail.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282RejShape.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282RejOwner.load(std::memory_order_relaxed),
                     (unsigned long long)g_g282VerifyBad.load(std::memory_order_relaxed));
    if (g283AuthorityOn() || g283StatOn() ||
        g_g283Bumps.load(std::memory_order_relaxed) != 0u)
        std::fprintf(stderr,
                     "[G283:stat] epochBumps=%llu stale=%llu multiOwner=%llu fanoutPages=%llu "
                     "pages=%zu\n",
                     (unsigned long long)g_g283Bumps.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283Stale.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283MultiOwner.load(std::memory_order_relaxed),
                     (unsigned long long)g_g283FanoutPages.load(std::memory_order_relaxed),
                     g_g283Epoch.size());
    if (g294StatOn() || g294OwnerTokenOn())
        std::fprintf(stderr,
                     "[G294:stat] ownerToken=%d stamps=%llu ownerSel=%llu nonOwnerSkip=%llu "
                     "owners=%zu\n",
                     g294OwnerTokenOn() ? 1 : 0,
                     (unsigned long long)g_g294Stamps.load(std::memory_order_relaxed),
                     (unsigned long long)g_g294OwnerSel.load(std::memory_order_relaxed),
                     (unsigned long long)g_g294NonOwnerSkip.load(std::memory_order_relaxed),
                     g_g294Owner.size());
    g309Report();
}
