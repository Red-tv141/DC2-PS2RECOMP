// ===== G766: THE SUB-WORD WRITE MIRROR — a guest write of ANY colour/index format into a
// =====       GPU-resident CT32 target is reconciled on the GPU, never by publishing the target =====
//
// THE WALL (plans/phase-G766-ledger.md §3). With the G766 page owners in place the remaining
// publications on the uncapped routes are WRITES, not reads:
//   * `lavaboss`: every frame a 64x64 PSMT8 IMAGE upload lands at 0x28a0 — half of one page inside
//     the dirty 448-row 0x139 surface — and the upload edge publishes the WHOLE surface (plus
//     0x13c and the narrow 0x139 view) so the 4 KiB write can compose over current bytes;
//   * `dragon`: the PSMT8 local->local copies into 0x3420 (0x139 rows 416..447) do the same, and
//     once G627 takes those, the PSMT8 uploads that follow them take their place.
// G264's upload mirror already avoids this for CT32 writes: it lets the write land in guest VRAM
// and patches exactly the written texels VRAM->FBO at the next flush edge. It refuses every other
// format because a narrower write replaces only PART of a CT32 word, and a part-word cannot be
// rebuilt from guest VRAM while the FBO is the authority for the rest (G626's "sub-word packing
// blocker"). G627 showed the blocker is only a CPU-side one for PSMT8 byte lanes. This file is the
// general form: ANY writer, ANY bit subset, one mechanism.
//
// THE RULE (one rule for every writer family, no title allowlist):
//   a guest write W into pages that a dirty resident target T holds is applied to guest VRAM as
//   the guest issued it, and T's FBO is patched  texel = (fbo & ~bits(W)) | (vram & bits(W))  at
//   T's next flush edge. Whole texels (bits == ~0) take G264's existing rectangle path; partial
//   texels take a two-pass AND/OR logic-op draw (g766_backend_write_color_rect_bits).
// Nothing is read back. After the patch the FBO holds the guest's bytes where W wrote and its own
// bytes everywhere else, which is exactly the truth, so T stays resident and dirty.
//
// EXACTNESS OF THE BIT SET. The bits a transfer shape writes are not derived from any swizzle
// description in this file (G594's law): they are PROBED once per shape by running the SHIPPED
// IMAGE writers (GSMem::WriteP8 / WriteP4 / WriteP4HL / ...) with all-ones values into a zeroed
// scratch copy of GS memory, with the SAME buffer-width conventions processImageData passes them.
// The probe is checked against the transfer's own bit count (rrw*rrh*bpp) and refused on any
// mismatch (overlap, wrap, an unknown writer), so a plan is either exact or absent.
//
// SAFETY OF THE DEFERRAL. Identical to G264's, because it rides G264's state: `mirPending`,
// `mirLo..mirHi`, the G597-scoped re-anchor and every flush point (materialize, waves, owner pulls,
// width flips) already exist and already run before any consumer reads the FBO or publishes it.
// Partial texels are admitted only inside the rows the FBO DEFINES (covLo..covHi): outside them
// the untouched bits of the FBO are undefined, and the target is published the legacy way.
// A partial texel never sets G291 coverage (VRAM is NOT current for its other bits).
//
// Local->local destinations use the same plan and the same deferred patch. Admission follows
// G627's conjuncts: the copy's SOURCE must be current in guest VRAM (no dirty target it reads, or
// G766's page test says guest VRAM already holds every page it reads), source and destination byte
// spans are disjoint, and the patch is noted only AFTER the CPU copy body has written guest VRAM
// (an FBO->FBO blit that takes the copy instead discards it).
//
//   DC2_G766_NO_MASKMIR=1     rollback: sub-word writes publish the target as before
//   DC2_G766_NO_MASKMIR_L2L=1 rollback of the local->local half only
//   DC2_G766_MASK_VERIFY=1    oracle: read every patched rect back, check written bits == VRAM and
//                             unwritten bits == the pre-patch FBO (perturbing; never timed)
//   DC2_G766_STAT=1           [G766:mm] census

static bool g766MaskMirOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_MASKMIR") &&
                           !envFlagDisabled("DC2_G766_MASKMIR");
    return on;
}
static bool g766MaskMirL2lOn()
{
    static const bool on = g766MaskMirOn() && !envFlagEnabled("DC2_G766_NO_MASKMIR_L2L");
    return on;
}
// The mirror takes EVERY format, CT32 included, whenever it can run: G264's CT32-only note keeps a
// per-(shape x all-target-layouts) plan cache that thrashes once residents stay dirty across the
// upload stream (11.9 % of the executor on lavaboss, g766g profile) — this plan is keyed on the
// transfer shape alone and mapped to each target per page.
static bool g766MaskMirActive()
{
    return g766MaskMirOn() && g264On() && g261WaveOn() && !g280AliasOn() && !g351SplitOn();
}
static bool g766MaskVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G766_MASK_VERIFY");
    return on;
}

static uint64_t g_g766MmNotes = 0u, g_g766MmTargets = 0u, g_g766MmFull = 0u, g_g766MmPart = 0u,
    g_g766MmNoHit = 0u, g_g766MmFlushes = 0u, g_g766MmFlushTexels = 0u, g_g766MmFail = 0u,
    g_g766MmRejPsm = 0u, g_g766MmRejPlan = 0u, g_g766MmRejCov = 0u, g_g766MmRejShape = 0u,
    g_g766MmDedup = 0u, g_g766MmVfyChecks = 0u, g_g766MmVfyBad = 0u, g_g766MmPlans = 0u,
    g_g766MmL2lTake = 0u, g_g766MmL2lRejSrc = 0u, g_g766MmL2lRejGeom = 0u, g_g766MmL2lApply = 0u,
    g_g766MmL2lDiscard = 0u, g_g766MmRowsSaved = 0u;

static void g766MmReport(bool force)
{
    if (!g766StatOn())
        return;
    static uint64_t s_n = 0u;
    if (!force && (++s_n % 1024u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G766:mm] notes=%llu targets=%llu texels(full=%llu part=%llu) nohit=%llu "
                 "flushes=%llu flushTexels=%llu fail=%llu dedup=%llu plans=%llu rowsSaved=%llu "
                 "rej(psm=%llu plan=%llu cov=%llu shape=%llu) l2l(take=%llu apply=%llu "
                 "discard=%llu rejSrc=%llu rejGeom=%llu) verify(checks=%llu bad=%llu)\n",
                 (unsigned long long)g_g766MmNotes, (unsigned long long)g_g766MmTargets,
                 (unsigned long long)g_g766MmFull, (unsigned long long)g_g766MmPart,
                 (unsigned long long)g_g766MmNoHit, (unsigned long long)g_g766MmFlushes,
                 (unsigned long long)g_g766MmFlushTexels, (unsigned long long)g_g766MmFail,
                 (unsigned long long)g_g766MmDedup, (unsigned long long)g_g766MmPlans,
                 (unsigned long long)g_g766MmRowsSaved,
                 (unsigned long long)g_g766MmRejPsm, (unsigned long long)g_g766MmRejPlan,
                 (unsigned long long)g_g766MmRejCov, (unsigned long long)g_g766MmRejShape,
                 (unsigned long long)g_g766MmL2lTake, (unsigned long long)g_g766MmL2lApply,
                 (unsigned long long)g_g766MmL2lDiscard, (unsigned long long)g_g766MmL2lRejSrc,
                 (unsigned long long)g_g766MmL2lRejGeom, (unsigned long long)g_g766MmVfyChecks,
                 (unsigned long long)g_g766MmVfyBad);
}

// ---- the write plan: which bits of which guest VRAM words one transfer shape writes -------------
struct G766WriteKey
{
    uint32_t dbp = 0u, dsax = 0u, dsay = 0u, rrw = 0u, rrh = 0u;
    uint8_t dbw = 0u, dpsm = 0u;
    bool operator==(const G766WriteKey &o) const
    {
        return dbp == o.dbp && dsax == o.dsax && dsay == o.dsay && rrw == o.rrw && rrh == o.rrh &&
               dbw == o.dbw && dpsm == o.dpsm;
    }
};
// PAGE-GRANULAR: most writers replace whole 8 KiB pages (every CT32 / T4 / T8 upload of page
// multiples), and those need no per-word mapping at all — a whole page lands on a whole 64x32 CT32
// tile of every resident layout, i.e. 32 rows of ONE 64-bit mirBits word. Only a partially written
// page carries its (word, bits) list. Per-target work is therefore O(pages) for page writes.
struct G766PlanPage
{
    uint32_t page = 0u;                 // guest GS page index
    uint32_t partBegin = 0u, partEnd = 0u; // [begin, end) into G766WritePlan::partWord/partBits;
                                           // empty = the WHOLE page is written (every bit)
};
struct G766WritePlan
{
    G766WriteKey key{};
    std::vector<G766PlanPage> pages;  // ascending page index
    std::vector<uint16_t> partWord;   // word index inside its page (0..2047)
    std::vector<uint32_t> partBits;   // the bits of that word the transfer writes
    uint64_t id = 0u;                 // unique per build (never reused), for the chunk de-dup
    uint64_t stamp = 0u;
    uint32_t hash = 0u;
    bool valid = false;
};

// Bits one texel of `psm` occupies in GS memory as the IMAGE writer stores it. CT24 is written
// through WriteCT32 with alpha 0x80 (processImageData's F62 quirk), i.e. a WHOLE word.
static uint32_t g766WriterBitsPerTexel(uint8_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT32: case GS_PSM_CT24: return 32u;
    case GS_PSM_CT16: case GS_PSM_CT16S: return 16u;
    case GS_PSM_T8: case GS_PSM_T8H: return 8u;
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: return 4u;
    default: return 0u; // Z formats and unknown PSMs: never admitted
    }
}

static uint32_t g766Popcount32(uint32_t v)
{
    uint32_t n = 0u;
    while (v != 0u)
    {
        v &= v - 1u;
        ++n;
    }
    return n;
}

// Probe: run the shipped writer over the whole rect with all-ones values into zeroed scratch GS
// memory, then read back which bits it set. Buffer-width conventions copied from processImageData:
// PSMT4 / PSMT8 pass dbw>>1 (min 1), PSMT4HL / PSMT4HH pass dbw verbatim, the rest pass dbw (min 1).
static bool g766BuildWritePlan(const G766WriteKey &k, G766WritePlan &out)
{
    const uint32_t bpt = g766WriterBitsPerTexel(k.dpsm);
    const uint64_t texels = static_cast<uint64_t>(k.rrw) * k.rrh;
    const uint32_t vramSize = g_g261VramSize;
    if (bpt == 0u || texels == 0u || texels > 1024ull * 1024ull || k.dbw == 0u ||
        vramSize == 0u || vramSize > (8u << 20))
        return false;
    const G144BlockRange rg = g144RangeForRect(k.dbp, k.dpsm, k.dbw, k.dsax, k.dsay, k.rrw, k.rrh);
    if (!rg.valid || rg.last < rg.first)
        return false;
    // Whole pages: the range helper's span, rounded out to page boundaries.
    const uint64_t lo = (static_cast<uint64_t>(rg.first) * 256ull) & ~8191ull;
    const uint64_t hi = ((static_cast<uint64_t>(rg.last) + 1ull) * 256ull + 8191ull) & ~8191ull;
    if (hi > vramSize)
        return false;
    static std::vector<uint8_t> s_scratch; // flush-owner thread only (the G144 contract)
    if (s_scratch.size() != vramSize)
        s_scratch.assign(vramSize, 0u);
    uint8_t *mem = s_scratch.data();
    std::memset(mem + lo, 0, static_cast<size_t>(hi - lo));
    const uint32_t bwHalf = (static_cast<uint32_t>(k.dbw) >> 1u) != 0u ? (k.dbw >> 1u) : 1u;
    const uint32_t bwOne = k.dbw != 0u ? k.dbw : 1u;
    for (uint32_t y = 0u; y < k.rrh; ++y)
        for (uint32_t x = 0u; x < k.rrw; ++x)
        {
            const uint32_t vx = k.dsax + x, vy = k.dsay + y;
            switch (k.dpsm)
            {
            case GS_PSM_CT32:
            case GS_PSM_CT24: GSMem::WriteCT32(mem, k.dbp, bwOne, vx, vy, 0xFFFFFFFFu); break;
            case GS_PSM_CT16: GSMem::WriteCT16(mem, k.dbp, bwOne, vx, vy, 0xFFFFu); break;
            case GS_PSM_CT16S: GSMem::WriteCT16S(mem, k.dbp, bwOne, vx, vy, 0xFFFFu); break;
            case GS_PSM_T8: GSMem::WriteP8(mem, k.dbp, bwHalf, vx, vy, 0xFFu); break;
            case GS_PSM_T8H: GSMem::WriteP8H(mem, k.dbp, bwOne, vx, vy, 0xFFu); break;
            case GS_PSM_T4: GSMem::WriteP4(mem, k.dbp, bwHalf, vx, vy, 0xFu); break;
            case GS_PSM_T4HL: GSMem::WriteP4HL(mem, k.dbp, k.dbw, vx, vy, 0xFu); break;
            case GS_PSM_T4HH: GSMem::WriteP4HH(mem, k.dbp, k.dbw, vx, vy, 0xFu); break;
            default: return false;
            }
        }
    out = G766WritePlan{};
    out.key = k;
    uint64_t setBits = 0u;
    for (uint64_t pageOff = lo; pageOff < hi; pageOff += 8192u)
    {
        const uint32_t *w = reinterpret_cast<const uint32_t *>(mem + pageOff);
        uint32_t full = 0u, any = 0u;
        for (uint32_t i = 0u; i < 2048u; ++i)
        {
            full += (w[i] == 0xFFFFFFFFu) ? 1u : 0u;
            any += (w[i] != 0u) ? 1u : 0u;
        }
        if (any == 0u)
            continue;
        G766PlanPage pp;
        pp.page = static_cast<uint32_t>(pageOff >> 13u);
        pp.partBegin = pp.partEnd = static_cast<uint32_t>(out.partWord.size());
        if (full == 2048u)
        {
            setBits += 2048ull * 32ull;
        }
        else
        {
            for (uint32_t i = 0u; i < 2048u; ++i)
                if (w[i] != 0u)
                {
                    out.partWord.push_back(static_cast<uint16_t>(i));
                    out.partBits.push_back(w[i]);
                    setBits += g766Popcount32(w[i]);
                }
            pp.partEnd = static_cast<uint32_t>(out.partWord.size());
        }
        out.pages.push_back(pp);
    }
    std::memset(mem + lo, 0, static_cast<size_t>(hi - lo));
    // The writer must have set EXACTLY rrw*rrh*bpp distinct bits inside the span. Fewer means a
    // write escaped the span (wrap) or two texels share storage; either way the plan is refused.
    if (setBits != texels * bpt)
        return false;
    static uint64_t s_id = 0u;
    out.id = ++s_id;
    out.valid = true;
    ++g_g766MmPlans;
    return true;
}

static uint32_t g766KeyHash(const G766WriteKey &k)
{
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix(k.dbp); mix(k.dsax); mix(k.dsay); mix(k.rrw); mix(k.rrh);
    mix((static_cast<uint32_t>(k.dbw) << 8) | k.dpsm);
    return h;
}

// LRU of 256 plans with a hash pre-filter (a route issues a few dozen distinct transfer shapes a
// frame; the old 48-entry cache thrashed on lavaboss's per-frame upload set).
static const G766WritePlan *g766GetWritePlan(const G766WriteKey &k)
{
    static std::vector<G766WritePlan> s_cache;
    static uint64_t s_stamp = 0u;
    const uint32_t h = g766KeyHash(k);
    for (G766WritePlan &p : s_cache)
        if (p.hash == h && p.key == k)
        {
            p.stamp = ++s_stamp;
            return p.valid ? &p : nullptr;
        }
    try
    {
        G766WritePlan plan;
        if (!g766BuildWritePlan(k, plan))
        {
            plan = G766WritePlan{};
            plan.key = k;
            plan.valid = false;
        }
        plan.hash = h;
        plan.stamp = ++s_stamp;
        if (s_cache.size() < 256u)
        {
            s_cache.push_back(std::move(plan));
            return s_cache.back().valid ? &s_cache.back() : nullptr;
        }
        auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                       [](const G766WritePlan &a, const G766WritePlan &b) {
                                           return a.stamp < b.stamp;
                                       });
        *oldest = std::move(plan);
        return oldest->valid ? &*oldest : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}

// ---- per-target mapping ------------------------------------------------------------------------
// Page `page`'s tile origin in slot t's CURRENT CT32 layout, or false when the slot's FBO does not
// represent it.
static inline bool g766PlanPageOrigin(uint32_t t, uint32_t page, uint32_t &x0, uint32_t &y0)
{
    const G261Res &r = g_g261Res[t];
    const uint32_t base = kG261Fbp[t];
    if (r.fbw == 0u || page < base)
        return false;
    const uint32_t off = page - base;
    const uint32_t row = off / r.fbw, col = off % r.fbw;
    if (row >= 16u)
        return false;
    x0 = col * 64u;
    y0 = row * 32u;
    return x0 + 64u <= static_cast<uint32_t>(r.fbW);
}

// Admission of ONE dirty target for a planned write: every partial texel must lie in rows the FBO
// defines. Returns false (legacy publication) otherwise. `hits` counts pages of this target.
static bool g766TargetAdmits(uint32_t t, const G766WritePlan &plan, const G264InvPage &inv,
                             uint32_t &hits)
{
    const G261Res &r = g_g261Res[t];
    hits = 0u;
    if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || r.fbW > 512 ||
        r.fbW != static_cast<int>(r.fbw) * 64)
    {
        ++g_g766MmRejShape;
        return false;
    }
    for (const G766PlanPage &pp : plan.pages)
    {
        uint32_t x0 = 0u, y0 = 0u;
        if (!g766PlanPageOrigin(t, pp.page, x0, y0))
            continue;
        ++hits;
        for (uint32_t k = pp.partBegin; k < pp.partEnd; ++k)
        {
            const int ty = static_cast<int>(y0 + inv.y[plan.partWord[k]]);
            if (ty < r.covLo || ty > r.covHi)
            {
                ++g_g766MmRejCov;
                return false;
            }
        }
    }
    return true;
}

// Record the plan's texels for slot t as a pending mirror (full texels -> mirBits, partial -> the
// bit list). Called only after g766TargetAdmits accepted the same plan for the same layout.
static void g766NotePlanForTarget(uint32_t t, const G766WritePlan &plan, const G264InvPage &inv)
{
    G261Res &r = g_g261Res[t];
    if (r.g766LastPlanId == plan.id && r.mirPending)
    {
        ++g_g766MmDedup; // a later chunk of the same IMAGE transfer: every texel is already listed
        return;
    }
    const bool g291On = g291AtlasOn();
    if (g291On && r.g291CovFbw != r.fbw)
    {
        r.g291Cov = {};
        r.g291CovFbw = r.fbw;
        g_g291LayoutResets.fetch_add(1u, std::memory_order_relaxed);
    }
    int lo = 512, hi = -1;
    for (const G766PlanPage &pp : plan.pages)
    {
        uint32_t x0 = 0u, y0 = 0u;
        if (!g766PlanPageOrigin(t, pp.page, x0, y0))
            continue;
        if (pp.partBegin == pp.partEnd)
        {
            // Whole page: one 64-bit word of 32 consecutive rows.
            const uint32_t wi = x0 >> 6u;
            for (uint32_t y = y0; y < y0 + 32u; ++y)
            {
                r.mirBits[y][wi] = ~uint64_t{0};
                if (g291On)
                    r.g291Cov[y][wi] = ~uint64_t{0}; // VRAM now holds every pixel of the page
            }
            g_g766MmFull += 2048u;
            lo = std::min(lo, static_cast<int>(y0));
            hi = std::max(hi, static_cast<int>(y0 + 31u));
            continue;
        }
        for (uint32_t k = pp.partBegin; k < pp.partEnd; ++k)
        {
            const uint32_t w = plan.partWord[k];
            const uint32_t tx = x0 + inv.x[w];
            const uint32_t ty = y0 + inv.y[w];
            const uint32_t m = plan.partBits[k];
            if (m == 0xFFFFFFFFu)
            {
                const uint64_t bit = uint64_t{1} << (tx & 63u);
                r.mirBits[ty][tx >> 6u] |= bit;
                if (g291On)
                    r.g291Cov[ty][tx >> 6u] |= bit;
                ++g_g766MmFull;
            }
            else
            {
                r.g766Mask.push_back(G766MaskTexel{static_cast<uint16_t>(tx),
                                                   static_cast<uint16_t>(ty), m});
                ++g_g766MmPart;
            }
            lo = std::min(lo, static_cast<int>(ty));
            hi = std::max(hi, static_cast<int>(ty));
        }
    }
    if (hi < lo)
        return;
    r.mirPending = true;
    if (lo < r.mirLo) r.mirLo = lo;
    if (hi > r.mirHi) r.mirHi = hi;
    r.g766LastPlanId = plan.id;
}

// The IMAGE upload edge. `skipMask` = targets G264 already deferred. Returns the targets whose
// publication this write no longer needs (their FBO will be patched instead).
static uint32_t g766NoteUploadMaskMirror(uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                         uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh,
                                         uint32_t skipMask)
{
    if (!g766MaskMirOn() || !g264On() || !g261WaveOn() || rrw == 0u || rrh == 0u ||
        g280AliasOn() || g351SplitOn())
        return 0u;
    const G144BlockRange up = g144RangeForRect(dbp, dpsm, dbw, dsax, dsay, rrw, rrh);
    if (!up.valid)
        return 0u;
    uint32_t cand = 0u;
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if ((skipMask & (1u << i)) == 0u && r.dirty && r.range.valid &&
            g144RangeOverlaps(r.range, up))
            cand |= 1u << i;
    }
    if (cand == 0u)
        return 0u;
    if (g766WriterBitsPerTexel(dpsm) == 0u)
    {
        ++g_g766MmRejPsm;
        return 0u;
    }
    const G264InvPage &inv = g264InvPage();
    const G766WritePlan *plan =
        inv.valid ? g766GetWritePlan(G766WriteKey{dbp, dsax, dsay, rrw, rrh, dbw, dpsm}) : nullptr;
    if (plan == nullptr)
    {
        ++g_g766MmRejPlan;
        return 0u;
    }
    ++g_g766MmNotes;
    uint32_t taken = 0u;
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        if ((cand & (1u << i)) == 0u)
            continue;
        uint32_t hits = 0u;
        if (!g766TargetAdmits(i, *plan, inv, hits))
            continue; // this target keeps its legacy publication
        const G261Res &r = g_g261Res[i];
        if (hits == 0u)
            ++g_g766MmNoHit; // the write lands in none of the target's words: nothing to reconcile
        else
            g766NotePlanForTarget(i, *plan, inv);
        taken |= 1u << i;
        ++g_g766MmTargets;
        if (r.dirtyHi >= r.dirtyLo)
            g_g766MmRowsSaved += static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
    }
    g766MmReport(false);
    return taken;
}

// ---- the flush: called from g264FlushMirror for the target it is reconciling --------------------
static void g766DropMaskMirror(G261Res &r)
{
    r.g766Mask.clear();
    r.g766LastPlanId = 0u;
}

static bool g766FlushMaskMirror(int ti, bool &any, int &lo, int &hi)
{
    any = false;
    G261Res &r = g_g261Res[ti];
    r.g766LastPlanId = 0u;
    if (r.g766Mask.empty())
        return true;
    uint8_t *vram = g_g144LastVram;
    if (vram == nullptr || r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || r.fbW > 512 ||
        g_g261VramSize == 0u)
    {
        r.g766Mask.clear();
        ++g_g766MmFail;
        return false;
    }
    int txLo = 512, txHi = -1, tyLo = 512, tyHi = -1;
    for (const G766MaskTexel &t : r.g766Mask)
    {
        txLo = std::min(txLo, static_cast<int>(t.tx));
        txHi = std::max(txHi, static_cast<int>(t.tx));
        tyLo = std::min(tyLo, static_cast<int>(t.ty));
        tyHi = std::max(tyHi, static_cast<int>(t.ty));
    }
    if (txHi < txLo || tyHi < tyLo || txHi >= r.fbW || tyHi >= 512)
    {
        r.g766Mask.clear();
        ++g_g766MmFail;
        return false;
    }
    const int w = txHi - txLo + 1;
    const int rows = tyHi - tyLo + 1;
    const int glY = 511 - tyHi; // GL rows are bottom-first: buffer row 0 is GS row tyHi
    const size_t n = static_cast<size_t>(w) * rows;
    static std::vector<uint32_t> s_val, s_bits, s_before; // flush-owner thread only
    s_val.assign(n, 0u);
    s_bits.assign(n, 0u);
    const uint32_t fbBlock = framePageBaseToBlock(kG261Fbp[ti]);
    for (const G766MaskTexel &t : r.g766Mask)
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, r.fbw, t.tx, t.ty);
        if (off + 4u > g_g261VramSize)
        {
            r.g766Mask.clear();
            ++g_g766MmFail;
            return false;
        }
        const size_t li = static_cast<size_t>(tyHi - t.ty) * w + static_cast<size_t>(t.tx - txLo);
        uint32_t word = 0u;
        std::memcpy(&word, vram + off, 4u);
        s_val[li] = word;
        s_bits[li] |= t.mask;
    }
    const size_t listed = r.g766Mask.size();
    r.g766Mask.clear();
    const bool verify = g766MaskVerifyOn();
    if (verify)
    {
        std::vector<uint32_t> pre;
        s_before.clear();
        if (g178_backend_read_color(kG261Fbp[ti], r.fbW, 512, glY, rows, pre) &&
            pre.size() >= static_cast<size_t>(r.fbW) * rows)
        {
            s_before.assign(n, 0u);
            for (int row = 0; row < rows; ++row)
                for (int c = 0; c < w; ++c)
                    s_before[static_cast<size_t>(row) * w + c] =
                        pre[static_cast<size_t>(row) * r.fbW + txLo + c];
        }
    }
    // G769: fire-and-forget unless the oracle must read the patch back right after it.
    const bool g769Async = g769AsyncMirOn() && !verify;
    if (g769Async
            ? !g769_backend_write_color_rect_bits_async(kG261Fbp[ti], r.fbW, 512, txLo, glY, w,
                                                        rows, std::vector<uint32_t>(s_val),
                                                        std::vector<uint32_t>(s_bits))
            : !g766_backend_write_color_rect_bits(kG261Fbp[ti], r.fbW, 512, txLo, glY, w, rows,
                                                  s_val, s_bits))
    {
        ++g_g766MmFail;
        std::fprintf(stderr, "[G766:mm-fail] backend t=0x%03x rect=%d,%d %dx%d fbW=%d\n",
                     kG261Fbp[ti], txLo, tyLo, w, rows, r.fbW);
        return false;
    }
    if (verify && !s_before.empty())
    {
        // Logical-resolution oracle: the readback resolves the centre tap of each logical texel.
        std::vector<uint32_t> after;
        if (g178_backend_read_color(kG261Fbp[ti], r.fbW, 512, glY, rows, after) &&
            after.size() >= static_cast<size_t>(r.fbW) * rows)
            for (int row = 0; row < rows; ++row)
                for (int c = 0; c < w; ++c)
                {
                    const size_t li = static_cast<size_t>(row) * w + c;
                    const uint32_t m = s_bits[li];
                    if (m == 0u)
                        continue;
                    const uint32_t got = after[static_cast<size_t>(row) * r.fbW + txLo + c];
                    const uint32_t want = (s_before[li] & ~m) | (s_val[li] & m);
                    ++g_g766MmVfyChecks;
                    if (got != want)
                    {
                        const uint64_t bad = ++g_g766MmVfyBad;
                        if (bad <= 16u)
                            std::fprintf(stderr,
                                         "[G766:mm-vfy-BAD] t=0x%03x tx=%d ty=%d mask=%08x "
                                         "before=%08x vram=%08x got=%08x want=%08x\n",
                                         kG261Fbp[ti], txLo + c, tyHi - row, m, s_before[li],
                                         s_val[li], got, want);
                    }
                }
    }
    ++g_g766MmFlushes;
    g_g766MmFlushTexels += listed;
    any = true;
    lo = tyLo;
    hi = tyHi;
    return true;
}

// ---- local->local destinations -----------------------------------------------------------------
// Decided at the l2l barrier (targets whose destination publication is skipped), applied after the
// CPU copy body has written guest VRAM, discarded when an FBO->FBO blit took the copy instead.
struct G766L2lPin
{
    bool armed = false;
    uint32_t mask = 0u;
    G766WriteKey key{};
};
static thread_local G766L2lPin t_g766L2lPin;

static void g766DisarmL2lPin()
{
    if (t_g766L2lPin.armed)
        ++g_g766MmL2lDiscard;
    t_g766L2lPin.armed = false;
}

// Byte span a transfer rect touches, from the same range helper every barrier uses.
static bool g766RectSpan(uint32_t bp, uint8_t bw, uint8_t psm, uint32_t sax, uint32_t say,
                         uint32_t rrw, uint32_t rrh, G144BlockRange &out)
{
    out = g144RangeForRect(bp, psm, bw, sax, say, rrw, rrh);
    return out.valid;
}

static uint32_t g766TryTakeL2lDest(uint32_t sbp, uint8_t sbw, uint8_t spsm,
                                   uint32_t ssax, uint32_t ssay,
                                   uint32_t dbp, uint8_t dbw, uint8_t dpsm,
                                   uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh,
                                   const G144BlockRange &src, const G144BlockRange &dst,
                                   bool srcUnknown, bool dstUnknown)
{
    t_g766L2lPin.armed = false;
    if (!g766MaskMirL2lOn() || !g264On() || !g261WaveOn() || !g261AnyDirty() || g280AliasOn() ||
        g351SplitOn())
        return 0u;
    if (srcUnknown || dstUnknown || !src.valid || !dst.valid || rrw == 0u || rrh == 0u ||
        spsm != dpsm || g766WriterBitsPerTexel(dpsm) == 0u || dpsm == GS_PSM_CT24 ||
        sbw == 0u || dbw == 0u)
    {
        ++g_g766MmL2lRejGeom;
        return 0u;
    }
    // Source and destination byte spans must be disjoint (the CPU body then reads only source
    // bytes that no part of the copy has written, whatever its traversal order).
    G144BlockRange s{}, d{};
    if (!g766RectSpan(sbp, sbw, spsm, ssax, ssay, rrw, rrh, s) ||
        !g766RectSpan(dbp, dbw, dpsm, dsax, dsay, rrw, rrh, d))
    {
        ++g_g766MmL2lRejGeom;
        return 0u;
    }
    if (g144RangeOverlaps(s, d))
    {
        // ⭐ G767: the block SPANS of two rects in the same buffer overlap whenever they share a
        // page, but for one (bp, bw, psm) the texel -> byte map is a bijection inside the buffer
        // width, so two DISJOINT texel rects of it write/read disjoint bytes — the traversal order
        // cannot matter. dungeon1 atlas-builds its PSMT8 textures this way (21,203 refusals/60 s).
        // Rollback DC2_G767_NO_L2LRECT=1.
        static const bool s_g767Rect = !envFlagEnabled("DC2_G767_NO_L2LRECT");
        const bool disjoint =
            s_g767Rect && sbp == dbp && sbw == dbw && spsm == dpsm &&
            ssax + rrw <= static_cast<uint32_t>(sbw) * 64u &&
            dsax + rrw <= static_cast<uint32_t>(dbw) * 64u &&
            (ssax + rrw <= dsax || dsax + rrw <= ssax || ssay + rrh <= dsay || dsay + rrh <= ssay);
        if (!disjoint)
        {
            ++g_g766MmL2lRejGeom;
            return 0u;
        }
    }
    uint32_t cand = 0u;
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        const G261Res &r = g_g261Res[i];
        if (!r.dirty)
            continue;
        // Every dirty target the SOURCE reads must already be current in guest VRAM for the
        // pages read (G627's conjunct 3 with G766's page test), or the CPU body copies stale
        // bytes: the whole edge keeps the legacy publication.
        if (!r.range.valid ||
            (g144RangeOverlaps(r.range, src) && !g766SrcPagesCurrent(i, src)))
        {
            ++g_g766MmL2lRejSrc;
            return 0u;
        }
        if (g144RangeOverlaps(r.range, dst))
            cand |= 1u << i;
    }
    if (cand == 0u)
        return 0u;
    const G264InvPage &inv = g264InvPage();
    const G766WriteKey key{dbp, dsax, dsay, rrw, rrh, dbw, dpsm};
    const G766WritePlan *plan = inv.valid ? g766GetWritePlan(key) : nullptr;
    if (plan == nullptr)
    {
        ++g_g766MmRejPlan;
        return 0u;
    }
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        if ((cand & (1u << i)) == 0u)
            continue;
        uint32_t hits = 0u;
        if (!g766TargetAdmits(i, *plan, inv, hits))
            return 0u; // all or nothing: the pin applies one plan to every taken target
    }
    t_g766L2lPin.armed = true;
    t_g766L2lPin.mask = cand;
    t_g766L2lPin.key = key;
    ++g_g766MmL2lTake;
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
        if (cand & (1u << i))
        {
            const G261Res &r = g_g261Res[i];
            if (r.dirtyHi >= r.dirtyLo)
                g_g766MmRowsSaved += static_cast<uint64_t>(r.dirtyHi - r.dirtyLo + 1);
        }
    return cand;
}

// After the CPU copy body: guest VRAM now holds the destination bytes. Note the deferred patch.
static void g766ApplyL2lPin()
{
    if (!t_g766L2lPin.armed)
        return;
    t_g766L2lPin.armed = false;
    const G766WritePlan *plan = g766GetWritePlan(t_g766L2lPin.key);
    const G264InvPage &inv = g264InvPage();
    for (uint32_t i = 0u; i < kG248TargetCount; ++i)
    {
        if ((t_g766L2lPin.mask & (1u << i)) == 0u)
            continue;
        G261Res &r = g_g261Res[i];
        if (plan == nullptr || !r.dirty)
        {
            // Cannot happen (nothing between the barrier and here clears it). If it ever does,
            // the only safe outcome is G264's loud failure: drop residency WITHOUT a readback.
            ++g_g766MmFail;
            std::fprintf(stderr, "[G766:mm-fail] l2l pin lost t=0x%03x plan=%d dirty=%d\n",
                         kG261Fbp[i], plan != nullptr ? 1 : 0, r.dirty ? 1 : 0);
            if (r.dirty)
            {
                g280DropOverlays(static_cast<int>(i));
                r.dirty = false;
                r.dirtyLo = 512;
                r.dirtyHi = -1;
                g261UpdateWindow(static_cast<int>(i));
                g310NoteTargetClean(false);
            }
            continue;
        }
        r.g766LastPlanId = 0u; // a copy is a distinct write even with an identical shape
        g766NotePlanForTarget(i, *plan, inv);
    }
    ++g_g766MmL2lApply;
    g766MmReport(false);
}

// ===== G766: TRANSIENT CONTRACTS SEEDED ON THE GPU — the prologue publication, deleted =====
//
// THE WALL (plans/phase-G766-ledger.md §4). After the write mirror, the largest remaining
// publication class on every uncapped route is `cpuReplay`: the prologue (`g735TrnPub`) of a
// TRANSIENT contract (G286 0x13b, G570 0x13d, G586 0x141, G587 0x15d, G604 discovered targets such
// as ridepod's 0x142) publishes every dirty resident sibling — the whole 448-row 0x139 window — so
// the transient FBO's seed upload reads current guest VRAM, then the transient renders and reads
// ITS result back into guest VRAM. On ridepod 82 % of these batches are untextured sprites: the
// only reason for the publication is that the transient's own surface lies inside 0x139's pages.
//
// THE REPLACEMENT, for a batch whose ONLY contact with dirty residents is its own colour surface:
//   1. no prologue publication;
//   2. the seed upload runs from guest VRAM as before, and every page of the transient surface whose
//      newest bytes live in a resident FBO (G766's valid page owner) is then pulled FBO->FBO into
//      the transient FBO before its first draw — so the transient renders over exactly the bytes
//      the publication would have given it;
//   3. its (unchanged) readback writes guest VRAM, bumps exactly the rows it wrote, and each dirty
//      resident overlapping the surface receives those rows through the deferred mirror (VRAM->FBO,
//      G264's contract, with the G597-scoped re-anchor) — so the residents stay resident AND current.
// Texture, CLUT or depth ranges that reach a dirty resident's non-current pages keep the legacy
// prologue. A pull that fails to arm or land sends the batch to the CPU fallback, whose own
// g261PrepareCpuReplay publishes the residents exactly as before.
//
//   DC2_G766_NO_TRNSEED=1   rollback: every transient runs the legacy prologue publication

struct G766TrnSeed
{
    bool armed = false;  // admitted at the prologue: no publication was made
    bool pulled = false; // pulls were armed for the render item
    bool gpuSeed = false; // G767: no VRAM upload — every rendered page is pulled from an FBO
    uint32_t fbp = 0u, fbw = 0u;
    int fbW = 0;
};
static G766TrnSeed s_g766Trn; // flush-owner thread only
static uint64_t g_g766TrnAdmit = 0u, g_g766TrnRefuse = 0u, g_g766TrnPulls = 0u,
    g_g766TrnPullPages = 0u, g_g766TrnFail = 0u, g_g766TrnWriteback = 0u, g_g766TrnRows = 0u;

static bool g766TrnSeedOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_TRNSEED") &&
                           !envFlagDisabled("DC2_G766_TRNSEED");
    return on;
}

static void g766TrnReport()
{
    if (!g766StatOn())
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 512u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G766:trn] admit=%llu refuse=%llu pulls=%llu pages=%llu fail=%llu "
                 "writeback=%llu rows=%llu\n",
                 (unsigned long long)g_g766TrnAdmit, (unsigned long long)g_g766TrnRefuse,
                 (unsigned long long)g_g766TrnPulls, (unsigned long long)g_g766TrnPullPages,
                 (unsigned long long)g_g766TrnFail, (unsigned long long)g_g766TrnWriteback,
                 (unsigned long long)g_g766TrnRows);
}

static void g766TransientReset()
{
    s_g766Trn = G766TrnSeed{};
    ++g_g767FlushSerial; // G767: per-flush memo epochs (page-view refusals)
}

static bool g766TransientArmed(uint32_t fbp)
{
    return s_g766Trn.armed && s_g766Trn.fbp == fbp;
}

// Prologue: true = admitted, the caller skips g735TrnPub.
static bool g766TransientSeedAdmit(uint32_t fbp)
{
    s_g766Trn = G766TrnSeed{};
    if (!g766TrnSeedOn() || !g766OwnerOn() || !g766PullsAllowed() || !g766MaskMirOn() ||
        !g264On() || !g261WaveOn() || !g261AnyDirty() || g_g144List.empty() ||
        s_g716Authority || g700On() || g280AliasOn() || g351SplitOn())
        return false;
    const GSContext &c0 = g_g144List[0].ctx;
    const uint32_t fbw = c0.frame.fbw;
    if (c0.frame.fbp != fbp || fbw == 0u || fbw > 8u || c0.frame.psm != GS_PSM_CT32)
    {
        ++g_g766TrnRefuse;
        return false;
    }
    // (A) Only a batch the legacy prologue would actually publish for is worth the pulls and the
    // write-back mirror; with no dirty resident under its surface or its textures the legacy path
    // costs nothing.
    bool anyOverlap = false;
    for (const G144Entry &e : g_g144List)
    {
        if (e.ctx.frame.fbp != fbp || e.ctx.frame.fbw != fbw || e.ctx.frame.psm != GS_PSM_CT32)
        {
            ++g_g766TrnRefuse;
            return false;
        }
        const G144BlockRange color = g144TargetRange(e);
        const G254Surface depth = g254DepthSurface(e);
        const bool tme = e.prim.tme;
        G144BlockRange tex{};
        if (tme && !g766TexFootprint(e, tex))
            tex = g144TextureRange(e);
        const G144BlockRange clut =
            (tme && g144IsPalettedPsm(e.ctx.tex0.psm)) ? g144ClutRange(e) : G144BlockRange{};
        if ((depth.active && !depth.range.valid) || (tme && !tex.valid))
        {
            ++g_g766TrnRefuse;
            return false;
        }
        for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        {
            const G261Res &r = g_g261Res[t];
            if (!r.dirty)
                continue;
            // A TEXTURE read of a dirty resident is not decided here: the batch's own bind pass
            // (FBO bind, T8 view, or its per-target publication) resolves it before any decode,
            // exactly as for a resident wave. CLUT and depth have no such pass.
            if (!r.range.valid || !color.valid ||
                (depth.active && g144RangeOverlaps(r.range, depth.range)) ||
                (clut.valid && g144RangeOverlaps(r.range, clut) && !g766SrcPagesCurrent(t, clut)))
            {
                ++g_g766TrnRefuse;
                return false;
            }
            if (g144RangeOverlaps(r.range, color) || (tme && g144RangeOverlaps(r.range, tex)))
                anyOverlap = true;
        }
    }
    if (!anyOverlap)
        return false;
    // Every dirty resident the transient surface overlaps must be able to take the write-back
    // mirror (whole CT32 texels only need the shape conjunct of g766TargetAdmits).
    const G144BlockRange win = g144RangeForRect(framePageBaseToBlock(fbp), GS_PSM_CT32, fbw, 0u,
                                                0u, fbw * 64u, 512u);
    if (!win.valid)
    {
        ++g_g766TrnRefuse;
        return false;
    }
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.dirty || !g144RangeOverlaps(r.range, win))
            continue;
        if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || r.fbW > 512 ||
            r.fbW != static_cast<int>(r.fbw) * 64)
        {
            ++g_g766TrnRefuse;
            return false;
        }
    }
    s_g766Trn.armed = true;
    s_g766Trn.fbp = fbp;
    s_g766Trn.fbw = fbw;
    s_g766Trn.fbW = static_cast<int>(fbw) * 64;
    ++g_g766TrnAdmit;
    g766TrnReport();
    return true;
}

// Before the synchronous submit: pull every page of the transient surface that a valid resident
// owner holds newer. False = the pulls could not be armed (the caller abandons the GPU path).
static bool g766TransientArmPulls(uint32_t fbp, uint32_t fboKey, int rowLo, int rowHi)
{
    if (!s_g766Trn.armed || s_g766Trn.fbp != fbp)
        return true;
    static std::vector<int32_t> s_rects; // flush-owner thread only
    s_rects.clear();
    uint32_t owners = 0u;
    uint32_t pages = 0u;
    const uint32_t fbw = s_g766Trn.fbw;
    // (B) Only the page rows this batch renders: its draws read nothing else of the transient
    // FBO, and its readback publishes a subset of these rows (G621).
    const uint32_t prLo = static_cast<uint32_t>(std::max(0, rowLo)) / 32u;
    const uint32_t prHi = static_cast<uint32_t>(std::min(511, std::max(0, rowHi))) / 32u;
    for (uint32_t pr = prLo; pr <= prHi && pr < 16u; ++pr)
        for (uint32_t pc = 0u; pc < fbw; ++pc)
        {
            const uint32_t page = fbp + pr * fbw + pc;
            if (page >= kG178PageCount)
                continue;
            int o = g766ValidOwner(page);
            if (o < 0 && s_g766Trn.gpuSeed)
            {
                // G767 GPU-only seed: no upload ran, so a page no FBO owns is pulled from a dirty
                // resident whose copy of it is current (== guest VRAM).
                for (uint32_t t = 0u; t < kG248TargetCount && o < 0; ++t)
                {
                    uint32_t r0 = 0u, c0 = 0u;
                    if (g_g261Res[t].dirty && kG261Fbp[t] != fbp && g767UnownedPageCurrent(t, page) &&
                        g766SlotPagePos(t, page, r0, c0))
                        o = static_cast<int>(t);
                }
                if (o < 0)
                    return false; // cannot happen after g767GpuSeedCovers; fail closed
            }
            if (o < 0)
                continue; // guest VRAM holds this page's newest bytes: the seed upload is exact
            uint32_t srow = 0u, scol = 0u;
            if (!g766SlotPagePos(static_cast<uint32_t>(o), page, srow, scol))
                return false;
            g766AppendRect(s_rects, g766SlotKey(static_cast<uint32_t>(o)), srow, scol, pr, pc);
            owners |= 1u << static_cast<uint32_t>(o);
            ++pages;
        }
    if (s_rects.empty())
        return true;
    g766FlushOwnerMirrors(owners);
    if (!g766_backend_arm_pulls(fboKey, s_rects))
    {
        ++g_g766TrnFail;
        return false;
    }
    s_g766Trn.pulled = true;
    ++g_g766TrnPulls;
    g_g766TrnPullPages += pages;
    return true;
}

// After the submit returned: true unless armed pulls did not land (the render saw a stale seed).
static bool g766TransientConfirm()
{
    if (!s_g766Trn.armed || !s_g766Trn.pulled)
        return true;
    if (g766_backend_last_pulls_applied())
        return true;
    ++g_g766TrnFail;
    std::fprintf(stderr, "[G766:trn-fail] transient 0x%03x pulls did not land; CPU fallback\n",
                 s_g766Trn.fbp);
    return false;
}

// After the readback wrote rows [lo, hi] of the transient surface into guest VRAM (and bumped
// exactly those rows): every dirty resident overlapping them takes the rows through the deferred
// mirror. All texels are whole CT32 words, so the G264 rectangle path carries them.
static void g766TransientWriteback(uint32_t fbp, uint32_t fbBlock, uint32_t fbw, int fbW, int lo,
                                   int hi)
{
    if (!s_g766Trn.armed || s_g766Trn.fbp != fbp)
        return;
    s_g766Trn.armed = false;
    if (hi < lo || fbW <= 0)
        return;
    (void)g766NoteUploadMaskMirror(fbBlock, static_cast<uint8_t>(fbw), GS_PSM_CT32, 0u,
                                   static_cast<uint32_t>(lo), static_cast<uint32_t>(fbW),
                                   static_cast<uint32_t>(hi - lo + 1), 0u);
    ++g_g766TrnWriteback;
    g_g766TrnRows += static_cast<uint64_t>(hi - lo + 1);
    g766TrnReport();
}

// ---- G766 lazy mirrors: the flush points (declared in g766_page_owner.inc) ----
static uint64_t g_g766LazyFlushes = 0u;
// Apply target t's pending mirror now; a failure takes G264's loud path (drop residency without
// a readback — the newer guest bytes must win).
static void g766MirrorFlushOrDrop(int ti)
{
    if (ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    G261Res &r = g_g261Res[ti];
    if (!r.mirPending)
        return;
    ++g_g766LazyFlushes;
    if (g264FlushMirror(ti))
        return;
    g280DropOverlays(ti);
    r.dirty = false;
    r.dirtyLo = 512;
    r.dirtyHi = -1;
    g261UpdateWindow(ti);
    G178FbSnap &snap = g_g178FbSnap[g766SlotSnapKey(static_cast<uint32_t>(ti))];
    snap.valid = false;
    snap.genSum = 0;
    snap.rowLo = 1 << 30;
    snap.rowHi = -1;
    g310NoteTargetClean(false);
}
static void g766MirrorFlushAll()
{
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        g766MirrorFlushOrDrop(static_cast<int>(t));
}
// Every slot whose FBO is keyed `key` (0x139's wide and narrow views share the address key).
static void g766MirrorFlushKey(uint32_t key)
{
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
        if (g_g261Res[t].mirPending && (g766SlotKey(t) == key || kG261Fbp[t] == key))
            g766MirrorFlushOrDrop(static_cast<int>(t));
}
