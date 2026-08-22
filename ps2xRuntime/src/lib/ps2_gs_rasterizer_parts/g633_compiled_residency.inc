// ===== G633: EVENT-DRIVEN COMPILED RESIDENCY ================================================
//
// G630 originally re-derived coherence on every draw: search the surface map, solve the sampled
// window, construct texture/CLUT ranges, scan page authority, allocate a view packet, then often
// discover that nothing changed. G633 moves those derivations to their state-change boundaries:
//
//   * G178 classification compiles immutable texture ranges and static admission once per state.
//   * page-authority transitions update subscribed plan counters;
//   * guest/raw writes invalidate only plans and surface mirrors whose physical pages overlap;
//   * CT32 consumer shapes are cached against direct surface-directory and surface generations;
//   * raw PSMT8 views remain resident until a subscribed source/CLUT page changes.
//
// A hot draw therefore reads a plan id, two generation/counter fields, and a resident token. The
// frozen G630 predicates remain the verification oracle and the fallback for any bounded-cache
// refusal. Arm with DC2_G633_PLANS=1; rollback DC2_G633_NO_PLANS=1.

static constexpr bool kG633PlansDefaultOn = false;
static const bool s_g633Stat = envFlagEnabled("DC2_G633_STAT");
static const bool s_g633Verify = envFlagEnabled("DC2_G633_VERIFY");
static const bool s_g633Configured = !envFlagEnabled("DC2_G633_NO_PLANS") &&
    (kG633PlansDefaultOn || envFlagEnabled("DC2_G633_PLANS") || s_g633Stat || s_g633Verify);

static bool g633PlansOn()
{
    return s_g633Configured && g630DomainOn();
}

enum : uint8_t
{
    kG633SubTexture = 1u,
    kG633SubClut = 2u,
};

struct G633TextureDesc
{
    uint32_t tbp = 0u, tbw = 0u, psm = 0u, tw = 0u, th = 0u;
    uint32_t cbp = 0u, cpsm = 0u, csm = 0u, csa = 0u;
    uint32_t ta0 = 0u, ta1 = 0u, aem = 0u;
    uint32_t cbw = 0u, cou = 0u, cov = 0u;
};

static G633TextureDesc g633TextureDesc(const G144Entry &e)
{
    G633TextureDesc d;
    d.tbp = e.ctx.tex0.tbp0; d.tbw = e.ctx.tex0.tbw; d.psm = e.ctx.tex0.psm;
    d.tw = e.ctx.tex0.tw; d.th = e.ctx.tex0.th;
    d.cbp = e.ctx.tex0.cbp; d.cpsm = e.ctx.tex0.cpsm;
    d.csm = e.ctx.tex0.csm; d.csa = e.ctx.tex0.csa;
    d.ta0 = e.texa.ta0; d.ta1 = e.texa.ta1; d.aem = e.texa.aem ? 1u : 0u;
    d.cbw = e.texclut.cbw; d.cou = e.texclut.cou; d.cov = e.texclut.cov;
    return d;
}

static bool g633TextureDescEqual(const G633TextureDesc &a, const G633TextureDesc &b)
{
    return a.tbp == b.tbp && a.tbw == b.tbw && a.psm == b.psm &&
           a.tw == b.tw && a.th == b.th && a.cbp == b.cbp &&
           a.cpsm == b.cpsm && a.csm == b.csm && a.csa == b.csa &&
           a.ta0 == b.ta0 && a.ta1 == b.ta1 && a.aem == b.aem &&
           a.cbw == b.cbw && a.cou == b.cou && a.cov == b.cov;
}

struct G633TexturePlan
{
    G633TextureDesc desc{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    uint64_t texKey = 0u, viewKey = 0u;
    int texW = 0, texH = 0;
    G144BlockRange texRange{}, clutRange{};
    std::array<uint64_t, 8> texBits{}, clutBits{};
    uint16_t texDirty = 0u, clutDirty = 0u;
    uint64_t textureGeneration = 1u, contentGeneration = 1u;
    uint64_t t8BuiltGeneration = 0u;
    uint64_t requiredPass = 0u, finalQueuedPass = 0u;
    G630Surface *ct32Surface = nullptr;
    uint64_t ct32ShapeGeneration = 0u, ct32PreparedGeneration = 0u;
    uint64_t ct32RequiredPass = 0u, ct32FinalQueuedPass = 0u;
    int ct32ViewLo = 512, ct32ViewHi = -1;
    bool t8Resident = false;
};

struct G633PageSub
{
    uint16_t plan = 0u;
    uint8_t roles = 0u;
};

static constexpr uint32_t kG633MaxTexturePlans = 4096u;
static std::vector<G633TexturePlan> g_g633TexturePlans;
static std::unordered_map<uint64_t, std::vector<uint16_t>> g_g633TextureByKey;
static std::array<std::vector<G633PageSub>, kG630Pages> g_g633PageSubs;

static std::array<uint16_t, 16384> g_g633SurfaceByBlock{}; // fbp + 1; zero means no owner
static std::array<uint64_t, 16384> g_g633BlockGeneration{};
static std::array<G630Surface *, 512> g_g633SurfaceByFbp{};
static std::array<uint64_t, 8> g_g633PendingGuestBits{};
static std::array<uint64_t, 512> g_g633SurfaceRequiredPass{};
static std::array<uint64_t, 512> g_g633SurfaceFinalQueuedPass{};
static uint64_t g_g633Pass = 1u;
static std::vector<uint16_t> g_g633FinalSurfaces;
static std::vector<uint32_t> g_g633FinalWindows;
static std::vector<uint16_t> g_g633FinalT8;

static uint64_t g_g633TextureLookups = 0u, g_g633TextureHits = 0u;
static uint64_t g_g633TextureCompiles = 0u, g_g633TextureRefused = 0u;
static uint64_t g_g633ConsumerProbes = 0u, g_g633ConsumerHits = 0u;
static uint64_t g_g633ConsumerMisses = 0u, g_g633ConsumerRevalidates = 0u;
static uint64_t g_g633BindRejects = 0u, g_g633BindFast = 0u, g_g633BindBuilds = 0u;
static uint64_t g_g633T8Rejects = 0u, g_g633T8Fast = 0u, g_g633T8Builds = 0u;
static uint64_t g_g633MaterializeFast = 0u, g_g633MaterializeBreaks = 0u;
static uint64_t g_g633AuthorityEvents = 0u, g_g633ContentEvents = 0u;
static uint64_t g_g633SurfaceEvents = 0u, g_g633FinalRepairs = 0u;
static uint64_t g_g633ConsumerDirtyEvents = 0u, g_g633ConsumerPublishes = 0u;
static uint64_t g_g633WindowCompiles = 0u;
static uint64_t g_g633GuestStages = 0u, g_g633GuestCommits = 0u;
static uint64_t g_g633GuestRawCancels = 0u;
static std::array<uint64_t, 32> g_g633GuestStagesBySource{};
static uint64_t g_g633VerifyBind = 0u, g_g633VerifyT8 = 0u;
static uint64_t g_g633VerifyAuthority = 0u, g_g633VerifyBad = 0u;

static G633TexturePlan *g633TexturePlan(uint32_t id)
{
    return id != 0u && id <= g_g633TexturePlans.size()
        ? &g_g633TexturePlans[id - 1u] : nullptr;
}

static const G633TexturePlan *g633TexturePlanConst(uint32_t id)
{
    return id != 0u && id <= g_g633TexturePlans.size()
        ? &g_g633TexturePlans[id - 1u] : nullptr;
}

static void g633RangeBits(const G144BlockRange &r, std::array<uint64_t, 8> &bits,
                          bool invalidMeansAll)
{
    bits.fill(0ull);
    if (!r.valid)
    {
        if (invalidMeansAll) bits.fill(~0ull);
        return;
    }
    const uint32_t lo = std::min(kG630Pages - 1u, r.first >> 5u);
    const uint32_t hi = std::min(kG630Pages - 1u, r.last >> 5u);
    for (uint32_t p = lo; p <= hi; ++p)
        bits[p >> 6u] |= 1ull << (p & 63u);
}

static bool g633BitsIntersect(const std::array<uint64_t, 8> &a,
                              const std::array<uint64_t, 8> &b)
{
    for (uint32_t i = 0u; i < 8u; ++i)
        if ((a[i] & b[i]) != 0ull) return true;
    return false;
}

static bool g633BitsSubset(const std::array<uint64_t, 8> &a,
                           const std::array<uint64_t, 8> &b)
{
    for (uint32_t i = 0u; i < 8u; ++i)
        if ((a[i] & ~b[i]) != 0ull) return false;
    return true;
}

static bool g633BitsAny(const std::array<uint64_t, 8> &bits)
{
    for (uint64_t word : bits)
        if (word != 0ull) return true;
    return false;
}

static void g633InvalidateSurfaceConsumers(G630Surface &s,
                                           const std::array<uint64_t, 8> &bits);
static void g633PublishSurfaceBits(G630Surface &s,
                                   const std::array<uint64_t, 8> &bits);
static void g633ResetConsumers();

static void g633RegisterPlanBits(uint16_t id, const std::array<uint64_t, 8> &bits,
                                 uint8_t role, uint16_t &dirty)
{
    dirty = 0u;
    for (uint32_t p = 0u; p < kG630Pages; ++p)
    {
        if ((bits[p >> 6u] & (1ull << (p & 63u))) == 0ull) continue;
        if (g_g630GpuDirty[p] != 0u) ++dirty;
        auto &subs = g_g633PageSubs[p];
        auto it = std::find_if(subs.begin(), subs.end(), [id](const G633PageSub &s) {
            return s.plan == id;
        });
        if (it == subs.end()) subs.push_back(G633PageSub{id, role});
        else it->roles |= role;
    }
}

static void g633QueueT8Repair(uint16_t id, G633TexturePlan &p)
{
    if (p.requiredPass != g_g633Pass || p.finalQueuedPass == g_g633Pass) return;
    p.finalQueuedPass = g_g633Pass;
    g_g633FinalT8.push_back(id);
}

static void g633QueueSurfaceRepair(uint32_t fbp)
{
    if (fbp >= g_g633SurfaceRequiredPass.size() ||
        g_g633SurfaceRequiredPass[fbp] != g_g633Pass ||
        g_g633SurfaceFinalQueuedPass[fbp] == g_g633Pass)
        return;
    g_g633SurfaceFinalQueuedPass[fbp] = g_g633Pass;
    g_g633FinalSurfaces.push_back(static_cast<uint16_t>(fbp));
}

static void g633InvalidateSurfaces(const std::array<uint64_t, 8> &bits, uint32_t mirroredFbp)
{
    for (auto &kv : g_g630Surfaces)
    {
        G630Surface &s = kv.second;
        if (!s.valid || s.fbp == mirroredFbp || !g633BitsIntersect(s.pageBits, bits)) continue;
        std::array<uint64_t, 8> changed{};
        for (uint32_t i = 0u; i < changed.size(); ++i)
        {
            changed[i] = s.residentBits[i] & bits[i];
            s.residentBits[i] &= ~bits[i];
        }
        if (!g633BitsAny(changed)) continue;
        ++s.generation;
        s.viewGeneration = 0u;
        ++g_g633SurfaceEvents;
        g633InvalidateSurfaceConsumers(s, changed);
    }
}

static void g633NotifyContentBits(const std::array<uint64_t, 8> &bits, uint32_t mirroredFbp)
{
    if (!s_g633Configured) return;
    for (uint32_t p = 0u; p < kG630Pages; ++p)
    {
        if ((bits[p >> 6u] & (1ull << (p & 63u))) == 0ull) continue;
        ++g_g633ContentEvents;
        for (const G633PageSub &sub : g_g633PageSubs[p])
        {
            G633TexturePlan *plan = g633TexturePlan(sub.plan);
            if (plan == nullptr) continue;
            if ((sub.roles & kG633SubTexture) != 0u)
            {
                // A G630 replay changes raw bytes and its own linear CT32 mirror in the same
                // dispatch. T8 content changes, but that source's CT32 equality remains true.
                const bool dualWritten = mirroredFbp != UINT32_MAX &&
                    plan->desc.psm == GS_PSM_CT32 &&
                    plan->desc.tbp == (mirroredFbp << 5u);
                if (!dualWritten)
                {
                    ++plan->textureGeneration;
                }
            }
            ++plan->contentGeneration;
            g633QueueT8Repair(sub.plan, *plan);
        }
    }
    g633InvalidateSurfaces(bits, mirroredFbp);
}

static void g633GuestWriteEvent(uint32_t basePage, uint32_t widthPages,
                                uint32_t py0, uint32_t py1,
                                uint32_t px0, uint32_t px1, bool exactAligned,
                                uint32_t source)
{
    if (!s_g633Configured || g_g630Fault) return;
    const uint32_t slack = exactAligned ? 0u : 1u;
    for (uint32_t py = py0; py <= py1; ++py)
        for (uint32_t px = px0; px <= px1 + slack; ++px)
        {
            const uint32_t p = (basePage + py * widthPages + px) & (kG630Pages - 1u);
            g_g633PendingGuestBits[p >> 6u] |= 1ull << (p & 63u);
        }
    ++g_g633GuestStages;
    if (s_g633Stat) ++g_g633GuestStagesBySource[std::min<uint32_t>(source, 31u)];
}

static void g633GuestWriteCommit()
{
    if (!s_g633Configured) return;
    if (g_g630Fault)
    {
        g_g633PendingGuestBits.fill(0ull);
        return;
    }
    bool any = false;
    for (uint64_t word : g_g633PendingGuestBits) any |= word != 0ull;
    if (!any) return;
    const std::array<uint64_t, 8> bits = g_g633PendingGuestBits;
    g_g633PendingGuestBits.fill(0ull);
    ++g_g633GuestCommits;
    g633NotifyContentBits(bits, UINT32_MAX);
}

static void g633AttachTexturePlan(const G144Entry &e, G178EntryState &st);

struct G633GuestHookInstaller
{
    G633GuestHookInstaller()
    {
        if (s_g633Configured)
        {
            g_g633GuestWriteHook = &g633GuestWriteEvent;
            g_g633GuestCommitHook = &g633GuestWriteCommit;
            g_g633AttachTextureHook = &g633AttachTexturePlan;
        }
    }
};
static G633GuestHookInstaller g_g633GuestHookInstaller;

static void g633AuthorityChanged(uint32_t page, bool dirty)
{
    if (!s_g633Configured || page >= kG630Pages) return;
    ++g_g633AuthorityEvents;
    for (const G633PageSub &sub : g_g633PageSubs[page])
    {
        G633TexturePlan *p = g633TexturePlan(sub.plan);
        if (p == nullptr) continue;
        if ((sub.roles & kG633SubTexture) != 0u)
        {
            if (dirty) ++p->texDirty;
            else if (p->texDirty != 0u) --p->texDirty;
        }
        if ((sub.roles & kG633SubClut) != 0u)
        {
            if (dirty) ++p->clutDirty;
            else if (p->clutDirty != 0u) --p->clutDirty;
        }
    }
}

static void g633AuthorityCleared()
{
    if (!s_g633Configured) return;
    for (G633TexturePlan &p : g_g633TexturePlans)
    {
        p.texDirty = 0u;
        p.clutDirty = 0u;
    }
}

static void g633CancelPendingGuestBits(const std::array<uint64_t, 8> &bits)
{
    bool canceled = false;
    for (uint32_t i = 0u; i < bits.size(); ++i)
    {
        canceled |= (g_g633PendingGuestBits[i] & bits[i]) != 0ull;
        g_g633PendingGuestBits[i] &= ~bits[i];
    }
    if (canceled) ++g_g633GuestRawCancels;
}

static void g633NoteRawRange(const G144BlockRange &range, uint32_t mirroredFbp)
{
    if (!s_g633Configured || !range.valid) return;
    std::array<uint64_t, 8> bits{};
    g633RangeBits(range, bits, false);
    // The upload is now ordered into persistent raw VRAM. Its earlier G178 guest marker describes
    // the same content transition, so consume those staged pages instead of publishing it twice.
    g633CancelPendingGuestBits(bits);
    g633NotifyContentBits(bits, mirroredFbp);
}

static void g633CommitReplaySurface(uint32_t fbp, uint32_t fbw, int height,
                                    uint32_t firstPage, uint32_t lastPage)
{
    std::array<uint64_t, 8> writeBits{};
    if (firstPage < kG630Pages && lastPage >= firstPage)
        for (uint32_t p = firstPage; p <= std::min(kG630Pages - 1u, lastPage); ++p)
            writeBits[p >> 6u] |= 1ull << (p & 63u);
    g633CancelPendingGuestBits(writeBits);
    g633NotifyContentBits(writeBits, fbp);

    G630Surface &s = g_g630Surfaces[fbp];
    const uint32_t block = fbp << 5u;
    const bool structural = !s.valid || s.block != block || s.fbw != fbw;
    const bool oldViewExact = s.valid && s.viewGeneration == s.generation;
    const int oldCovHi = s.covHi;
    if (structural)
    {
        if (s.valid && s.block < g_g633SurfaceByBlock.size() &&
            g_g633SurfaceByBlock[s.block] == s.fbp + 1u)
        {
            g_g633SurfaceByBlock[s.block] = 0u;
            ++g_g633BlockGeneration[s.block];
        }
        s.pageBits.fill(0ull);
        s.residentBits.fill(0ull);
        s.covLo = 512; s.covHi = -1;
        s.viewLo = 512; s.viewHi = -1;
        s.generation = std::max<uint64_t>(1u, s.generation + 1u);
        ++s.shapeGeneration;
        g_g633SurfaceByBlock[block] = static_cast<uint16_t>(fbp + 1u);
        ++g_g633BlockGeneration[block];
    }
    else
        ++s.generation;

    for (uint32_t i = 0u; i < 8u; ++i) s.pageBits[i] |= writeBits[i];
    s.fbp = fbp; s.block = block; s.fbw = fbw; s.fbW = static_cast<int>(fbw) * 64;
    s.covLo = 0; s.covHi = std::max(s.covHi, height - 1); s.seq = ++g_g630Seq; s.valid = true;
    if (!structural && s.covHi != oldCovHi) ++s.shapeGeneration;
    s.viewSeq = s.seq;
    s.viewLo = 0;
    s.viewHi = (!structural && oldViewExact)
        ? std::max(s.viewHi, std::min(511, height - 1))
        : std::min(511, height - 1);
    s.viewGeneration = s.generation;
    s.viewGlobalGen = g_g149VramGen.load(std::memory_order_relaxed);
    s.viewGen = 0u;
    for (uint32_t p = firstPage; p <= std::min(kG630Pages - 1u, lastPage); ++p)
        s.viewGen += g_g178PageGen[p].load(std::memory_order_relaxed);
    if (fbp < g_g633SurfaceByFbp.size()) g_g633SurfaceByFbp[fbp] = &s;
    g633PublishSurfaceBits(s, writeBits);
    // The replay writes raw VRAM and this linear mirror in the same FIFO dispatch. Any compiled
    // CT32 plan wholly covered by that write is resident at the new texture generation without a
    // raw-to-FBO rebuild.
    for (G633TexturePlan &p : g_g633TexturePlans)
    {
        if (p.desc.psm != GS_PSM_CT32 || p.desc.tbp != block || p.desc.tbw != fbw)
            continue;
        const bool wasResident = p.ct32Surface == &s &&
                                 p.ct32PreparedGeneration == p.textureGeneration;
        if (wasResident || g633BitsSubset(p.texBits, writeBits))
        {
            p.ct32Surface = &s;
            p.ct32ShapeGeneration = s.shapeGeneration;
            p.ct32PreparedGeneration = p.textureGeneration;
            p.ct32ViewLo = 0;
            p.ct32ViewHi = wasResident
                ? std::max(p.ct32ViewHi, std::min(511, height - 1))
                : std::min(511, height - 1);
        }
    }
}

static int g633SurfaceFbW(uint32_t fbp)
{
    if (fbp >= g_g633SurfaceByFbp.size()) return 0;
    const G630Surface *s = g_g633SurfaceByFbp[fbp];
    return s != nullptr && s->valid ? s->fbW : 0;
}

static void g633ResetDomain()
{
    if (!s_g633Configured) return;
    g_g633SurfaceByBlock.fill(0u);
    g_g633SurfaceByFbp.fill(nullptr);
    g_g633PendingGuestBits.fill(0ull);
    for (uint64_t &g : g_g633BlockGeneration) ++g;
    for (auto &kv : g_g630Surfaces) kv.second.residentBits.fill(0ull);
    g633ResetConsumers();
    for (G633TexturePlan &p : g_g633TexturePlans)
    {
        p.t8Resident = false;
        p.t8BuiltGeneration = 0u;
        p.ct32Surface = nullptr;
        p.ct32PreparedGeneration = 0u;
        p.ct32ViewLo = 512; p.ct32ViewHi = -1;
        ++p.textureGeneration;
        ++p.contentGeneration;
    }
}

static uint32_t g633FindOrCompileTexturePlan(const G144Entry &e, const G178EntryState &st)
{
    ++g_g633TextureLookups;
    const G633TextureDesc desc = g633TextureDesc(e);
    auto found = g_g633TextureByKey.find(st.texKey);
    if (found != g_g633TextureByKey.end())
        for (uint16_t id : found->second)
        {
            const G633TexturePlan *p = g633TexturePlanConst(id);
            if (p != nullptr && g633TextureDescEqual(p->desc, desc))
            {
                ++g_g633TextureHits;
                return id;
            }
        }
    if (g_g633TexturePlans.size() >= kG633MaxTexturePlans)
    {
        ++g_g633TextureRefused;
        return 0u;
    }
    if (g_g633TexturePlans.capacity() == 0u) g_g633TexturePlans.reserve(256u);
    G633TexturePlan p;
    p.desc = desc;
    p.texa = e.texa;
    p.texclut = e.texclut;
    p.texKey = st.texKey;
    p.viewKey = g281ViewKey(st.texKey);
    p.texW = st.texW; p.texH = st.texH;
    p.texRange = g144TextureRange(e);
    if (g144IsPalettedPsm(e.ctx.tex0.psm)) p.clutRange = g144ClutRange(e);
    g633RangeBits(p.texRange, p.texBits, !p.texRange.valid);
    g633RangeBits(p.clutRange, p.clutBits, false);
    g_g633TexturePlans.push_back(p);
    const uint16_t id = static_cast<uint16_t>(g_g633TexturePlans.size());
    G633TexturePlan &stored = g_g633TexturePlans.back();
    g633RegisterPlanBits(id, stored.texBits, kG633SubTexture, stored.texDirty);
    g633RegisterPlanBits(id, stored.clutBits, kG633SubClut, stored.clutDirty);
    g_g633TextureByKey[st.texKey].push_back(id);
    ++g_g633TextureCompiles;
    return id;
}

static void g633AttachTexturePlan(const G144Entry &e, G178EntryState &st)
{
    st.residencyPlan = 0u;
    st.residencyBindMask = 0u;
    st.residencyT8Mask = 0u;
    if (!s_g633Configured || !e.prim.tme || st.texKey == 0u) return;
    st.residencyPlan = g633FindOrCompileTexturePlan(e, st);
    if (st.residencyPlan == 0u) return;
    if (e.ctx.tex0.psm != GS_PSM_CT32) st.residencyBindMask |= kG632BindPsm;
    if (!e.prim.fst) st.residencyBindMask |= kG632BindNoFst;
    if (e.ctx.tex0.psm != GS_PSM_T8) st.residencyT8Mask |= kG632T8Psm;
    if (st.texKey == 0u) st.residencyT8Mask |= kG632T8NoKey;
    if (st.texW <= 0 || st.texH <= 0 || st.texW > 1024 || st.texH > 1024)
        st.residencyT8Mask |= kG632T8Dim;
}

static void g633ExpandBits(const std::array<uint64_t, 8> &bits,
                           std::array<uint8_t, kG630Pages> &wanted)
{
    for (uint32_t p = 0u; p < kG630Pages; ++p)
        wanted[p] = ((bits[p >> 6u] >> (p & 63u)) & 1ull) != 0ull ? 1u : 0u;
}

static bool g633MaterializePlanRole(uint32_t id, uint8_t role, int cause)
{
    G633TexturePlan *p = g633TexturePlan(id);
    if (!g633PlansOn() || p == nullptr) return false;
    const uint16_t dirty = role == kG633SubTexture ? p->texDirty : p->clutDirty;
    const auto &bits = role == kG633SubTexture ? p->texBits : p->clutBits;
    if (s_g633Verify)
    {
        bool legacy = false;
        for (uint32_t w = 0u; w < 8u; ++w)
            if ((bits[w] & g_g630DirtyBits[w]) != 0ull) { legacy = true; break; }
        ++g_g633VerifyAuthority;
        if (legacy != (dirty != 0u)) ++g_g633VerifyBad;
    }
    if (dirty == 0u)
    {
        ++g_g633MaterializeFast;
        return true;
    }
    std::array<uint8_t, kG630Pages> wanted{};
    g633ExpandBits(bits, wanted);
    const bool hit = true;
    if (g_g632Ep.open && g632EpochAdd(wanted, hit, cause)) return true;
    ++g_g633MaterializeBreaks;
    g_g630CpuBreaks.fetch_add(1u, std::memory_order_relaxed);
    (void)g630ReadAndPublish(wanted, cause);
    return true;
}

static bool g633MaterializeTexturePlan(uint32_t id, int cause)
{
    return g633MaterializePlanRole(id, kG633SubTexture, cause);
}

static bool g633MaterializeClutPlan(uint32_t id, int cause)
{
    return g633MaterializePlanRole(id, kG633SubClut, cause);
}

static bool g633TexturePlanHasAuthority(uint32_t id)
{
    const G633TexturePlan *p = g633TexturePlanConst(id);
    return p != nullptr && p->texDirty != 0u;
}

struct G633ConsumerKey
{
    uint32_t texturePlan = 0u, dstFbp = 0u;
    uint32_t prim = 0u, bilinear = 0u, wrapU = 0u, wrapV = 0u;
    uint32_t xy[4]{};
    uint32_t uv[6]{};
};

struct G633ConsumerPlan
{
    G633ConsumerKey key{};
    uint64_t hash = 0u, blockGeneration = 0u, surfaceGeneration = 0u;
    G630Surface *surface = nullptr;
    uint32_t windowPlan = 0u;
    uint32_t mask = kG632BindNoSurface;
    int viewLo = 0, viewHi = -1;
    bool occupied = false;
};

struct G633ResidencyWindow
{
    G630Surface *surface = nullptr;
    std::array<uint64_t, 8> sampleBits{}, dirtyBits{};
    uint64_t surfaceGeneration = 0u, requiredPass = 0u, finalQueuedPass = 0u;
    uint16_t dirtyPages = 0u;
    int viewLo = 0, viewHi = -1;
};

static constexpr uint32_t kG633ConsumerSlots = 65536u;
static constexpr uint32_t kG633WindowBands = 16u;
static constexpr uint32_t kG633WindowSlots = 512u * kG633WindowBands * kG633WindowBands;
static std::array<G633ConsumerPlan, kG633ConsumerSlots> g_g633Consumers{};
static std::vector<G633ResidencyWindow> g_g633Windows;
static std::array<uint32_t, kG633WindowSlots> g_g633WindowBySurfaceBands{};
static std::array<std::vector<uint32_t>, kG630Pages> g_g633WindowPageSubs;

static G633ResidencyWindow *g633WindowPlan(uint32_t id)
{
    return id != 0u && id <= g_g633Windows.size() ? &g_g633Windows[id - 1u] : nullptr;
}

static bool g633WindowSamplesPage(const G633ResidencyWindow &w, uint32_t page)
{
    return page < kG630Pages &&
           (w.sampleBits[page >> 6u] & (1ull << (page & 63u))) != 0ull;
}

static void g633QueueWindowRepair(uint32_t id, G633ResidencyWindow &w)
{
    if (w.requiredPass != g_g633Pass || w.finalQueuedPass == g_g633Pass) return;
    w.finalQueuedPass = g_g633Pass;
    g_g633FinalWindows.push_back(id);
}

static void g633InvalidateSurfaceConsumers(G630Surface &s,
                                           const std::array<uint64_t, 8> &bits)
{
    for (uint32_t page = 0u; page < kG630Pages; ++page)
    {
        const uint64_t bit = 1ull << (page & 63u);
        if ((bits[page >> 6u] & bit) == 0ull) continue;
        auto &subs = g_g633WindowPageSubs[page];
        for (size_t i = 0u; i < subs.size();)
        {
            const uint32_t id = subs[i];
            G633ResidencyWindow *w = g633WindowPlan(id);
            if (w == nullptr || w->surface == nullptr || !w->surface->valid ||
                w->surface->shapeGeneration != w->surfaceGeneration ||
                !g633WindowSamplesPage(*w, page))
            {
                subs[i] = subs.back();
                subs.pop_back();
                continue;
            }
            if (w->surface == &s && (w->dirtyBits[page >> 6u] & bit) == 0ull)
            {
                w->dirtyBits[page >> 6u] |= bit;
                ++w->dirtyPages;
                ++g_g633ConsumerDirtyEvents;
                g633QueueWindowRepair(id, *w);
            }
            ++i;
        }
    }
}

static void g633PublishSurfaceBits(G630Surface &s,
                                   const std::array<uint64_t, 8> &bits)
{
    for (uint32_t i = 0u; i < s.residentBits.size(); ++i)
        s.residentBits[i] |= bits[i];
    for (uint32_t page = 0u; page < kG630Pages; ++page)
    {
        const uint64_t bit = 1ull << (page & 63u);
        if ((bits[page >> 6u] & bit) == 0ull) continue;
        for (uint32_t id : g_g633WindowPageSubs[page])
        {
            G633ResidencyWindow *w = g633WindowPlan(id);
            if (w == nullptr || w->surface != &s ||
                w->surfaceGeneration != s.shapeGeneration ||
                !g633WindowSamplesPage(*w, page) ||
                (w->dirtyBits[page >> 6u] & bit) == 0ull)
                continue;
            w->dirtyBits[page >> 6u] &= ~bit;
            if (w->dirtyPages != 0u) --w->dirtyPages;
            ++g_g633ConsumerPublishes;
        }
    }
}

static void g633ResetConsumers()
{
    for (G633ConsumerPlan &cp : g_g633Consumers) cp = G633ConsumerPlan{};
    g_g633Windows.clear();
    g_g633WindowBySurfaceBands.fill(0u);
    for (auto &subs : g_g633WindowPageSubs) subs.clear();
    g_g633FinalWindows.clear();
}

static void g633CompileWindowBits(const G630Surface &s, int viewLo, int viewHi,
                                  std::array<uint64_t, 8> &bits)
{
    bits.fill(0ull);
    if (s.fbw == 0u || viewHi < viewLo) return;
    const uint32_t basePage = s.block >> 5u;
    const uint32_t pageY0 = static_cast<uint32_t>(std::clamp(viewLo, 0, 511)) >> 5u;
    const uint32_t pageY1 = static_cast<uint32_t>(std::clamp(viewHi, 0, 511)) >> 5u;
    for (uint32_t py = pageY0; py <= pageY1; ++py)
        for (uint32_t px = 0u; px < s.fbw; ++px)
        {
            const uint32_t page = (basePage + py * s.fbw + px) & (kG630Pages - 1u);
            bits[page >> 6u] |= 1ull << (page & 63u);
        }
}

static uint16_t g633CountBits(const std::array<uint64_t, 8> &bits)
{
    uint16_t count = 0u;
    for (uint64_t word : bits)
        while (word != 0ull)
        {
            word &= word - 1ull;
            ++count;
        }
    return count;
}

static uint32_t g633FindOrCompileWindow(G630Surface &s, int viewLo, int viewHi)
{
    if (s.fbp >= 512u || s.fbw == 0u || viewHi < viewLo) return 0u;
    const uint32_t bandLo = static_cast<uint32_t>(std::clamp(viewLo, 0, 511)) >> 5u;
    const uint32_t bandHi = static_cast<uint32_t>(std::clamp(viewHi, 0, 511)) >> 5u;
    const uint32_t slot = s.fbp * kG633WindowBands * kG633WindowBands +
                          bandLo * kG633WindowBands + bandHi;
    const uint32_t existing = g_g633WindowBySurfaceBands[slot];
    G633ResidencyWindow *current = g633WindowPlan(existing);
    if (current != nullptr && current->surface == &s &&
        current->surfaceGeneration == s.shapeGeneration)
        return existing;
    if (g_g633Windows.capacity() == 0u) g_g633Windows.reserve(1024u);
    G633ResidencyWindow w;
    w.surface = &s;
    w.surfaceGeneration = s.shapeGeneration;
    w.viewLo = static_cast<int>(bandLo * 32u);
    w.viewHi = static_cast<int>(bandHi * 32u + 31u);
    g633CompileWindowBits(s, w.viewLo, w.viewHi, w.sampleBits);
    for (uint32_t i = 0u; i < w.sampleBits.size(); ++i)
    {
        w.dirtyBits[i] = w.sampleBits[i] & ~s.residentBits[i];
    }
    w.dirtyPages = g633CountBits(w.dirtyBits);
    g_g633Windows.push_back(w);
    const uint32_t id = static_cast<uint32_t>(g_g633Windows.size());
    g_g633WindowBySurfaceBands[slot] = id;
    for (uint32_t page = 0u; page < kG630Pages; ++page)
    {
        if (!g633WindowSamplesPage(g_g633Windows.back(), page)) continue;
        g_g633WindowPageSubs[page].push_back(id);
    }
    ++g_g633WindowCompiles;
    return id;
}

static uint32_t g633FloatBits(float value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static G633ConsumerKey g633ConsumerKey(const G144Entry &e, const G178EntryState &st)
{
    G633ConsumerKey k;
    k.texturePlan = st.residencyPlan;
    k.dstFbp = e.ctx.frame.fbp;
    k.prim = static_cast<uint32_t>(e.prim.type);
    k.bilinear = st.bilinear ? 1u : 0u;
    k.wrapU = st.wrapU; k.wrapV = st.wrapV;
    const int nv = e.prim.type == GS_PRIM_SPRITE ? 2 : 3;
    for (int i = 0; i < nv; ++i)
    {
        k.uv[i * 2] = e.v[i].u;
        k.uv[i * 2 + 1] = e.v[i].v;
    }
    if (st.bilinear && e.prim.type == GS_PRIM_SPRITE)
    {
        k.xy[0] = g633FloatBits(e.v[0].x); k.xy[1] = g633FloatBits(e.v[0].y);
        k.xy[2] = g633FloatBits(e.v[1].x); k.xy[3] = g633FloatBits(e.v[1].y);
    }
    return k;
}

static bool g633ConsumerKeyEqual(const G633ConsumerKey &a, const G633ConsumerKey &b)
{
    return a.texturePlan == b.texturePlan && a.dstFbp == b.dstFbp &&
           a.prim == b.prim && a.bilinear == b.bilinear &&
           a.wrapU == b.wrapU && a.wrapV == b.wrapV &&
           std::equal(std::begin(a.xy), std::end(a.xy), std::begin(b.xy)) &&
           std::equal(std::begin(a.uv), std::end(a.uv), std::begin(b.uv));
}

static uint64_t g633ConsumerHash(const G633ConsumerKey &k)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint32_t v) { h ^= v; h *= 1099511628211ull; };
    mix(k.texturePlan); mix(k.dstFbp); mix(k.prim); mix(k.bilinear); mix(k.wrapU); mix(k.wrapV);
    for (uint32_t v : k.xy) mix(v);
    for (uint32_t v : k.uv) mix(v);
    // FST UVs are 12.4 fixed point, so their low four bits are usually zero. FNV is a sound key
    // hash but its low bits preserve that alignment; avalanche before the power-of-two table index
    // or unrelated mesh triangles collapse onto the same slots.
    h ^= h >> 33u;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33u;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33u;
    return h;
}

static uint32_t g633EvaluateConsumer(const G144Entry &e, const G178EntryState &st,
                                     G630Surface *sp, const G144BlockRange &range,
                                     int &viewLo, int &viewHi)
{
    if (sp == nullptr) return kG632BindNoSurface;
    uint32_t m = 0u;
    if (sp->fbp == e.ctx.frame.fbp) m |= kG632BindSelf;
    if (st.texW <= 0 || st.texH <= 0) m |= kG632BindTexDim;
    if (st.texW > sp->fbW) m |= kG632BindTexWide;
    if (sp->covHi < sp->covLo) m |= kG632BindNoCov;
    bool window = false;
    viewLo = 0; viewHi = -1;
    if (st.bilinear && e.prim.type == GS_PRIM_SPRITE)
    {
        G623Surface tmp{};
        tmp.fbW = sp->fbW; tmp.fbH = 512; tmp.covLo = sp->covLo; tmp.covHi = sp->covHi;
        int taps[4]{};
        window = g623BilinearSpriteWindow(e, st, tmp, taps);
        if (window) { viewLo = taps[2]; viewHi = taps[3]; }
    }
    else if (!st.bilinear)
    {
        const int nv = e.prim.type == GS_PRIM_SPRITE ? 2 : 3;
        int loU = INT_MAX, hiU = INT_MIN, loV = INT_MAX, hiV = INT_MIN;
        for (int i = 0; i < nv; ++i)
        {
            loU = std::min(loU, static_cast<int>(e.v[i].u >> 4));
            hiU = std::max(hiU, static_cast<int>(e.v[i].u >> 4));
            loV = std::min(loV, static_cast<int>(e.v[i].v >> 4));
            hiV = std::max(hiV, static_cast<int>(e.v[i].v >> 4));
        }
        if (e.prim.type == GS_PRIM_SPRITE) { --hiU; --hiV; }
        window = loU >= 0 && hiU < sp->fbW && loV >= sp->covLo && hiV <= sp->covHi;
        if (window) { viewLo = loV; viewHi = hiV; }
    }
    if (!window) m |= kG632BindWindow;
    if (!range.valid || viewHi < viewLo) m |= kG632BindRange;
    return m;
}

static G633ConsumerPlan &g633ResolveConsumer(const G144Entry &e, const G178EntryState &st)
{
    ++g_g633ConsumerProbes;
    const G633ConsumerKey key = g633ConsumerKey(e, st);
    const uint64_t hash = g633ConsumerHash(key);
    const uint16_t slot = static_cast<uint16_t>(hash & (kG633ConsumerSlots - 1u));
    G633ConsumerPlan &cp = g_g633Consumers[slot];
    const G633TexturePlan *tp = g633TexturePlanConst(st.residencyPlan);
    const uint32_t block = tp != nullptr ? tp->desc.tbp : 0u;
    const uint64_t blockGen = block < g_g633BlockGeneration.size()
        ? g_g633BlockGeneration[block] : 0u;
    const bool keyHit = cp.occupied && cp.hash == hash && g633ConsumerKeyEqual(cp.key, key);
    const bool surfaceCurrent = cp.surface == nullptr ||
        (cp.surface->valid && cp.surface->shapeGeneration == cp.surfaceGeneration);
    if (keyHit && cp.blockGeneration == blockGen && surfaceCurrent)
    {
        ++g_g633ConsumerHits;
        return cp;
    }
    if (keyHit) ++g_g633ConsumerRevalidates;
    else ++g_g633ConsumerMisses;
    cp = G633ConsumerPlan{};
    cp.occupied = true; cp.hash = hash; cp.key = key; cp.blockGeneration = blockGen;
    if (tp == nullptr || block >= g_g633SurfaceByBlock.size()) return cp;
    const uint16_t encoded = g_g633SurfaceByBlock[block];
    if (encoded == 0u) return cp;
    const uint32_t fbp = encoded - 1u;
    G630Surface *sp = fbp < g_g633SurfaceByFbp.size() ? g_g633SurfaceByFbp[fbp] : nullptr;
    if (sp == nullptr || !sp->valid || sp->fbw != tp->desc.tbw) return cp;
    cp.surface = sp;
    cp.surfaceGeneration = sp->shapeGeneration;
    cp.mask = g633EvaluateConsumer(e, st, sp, tp->texRange, cp.viewLo, cp.viewHi);
    if (cp.mask == 0u)
    {
        cp.windowPlan = g633FindOrCompileWindow(*sp, cp.viewLo, cp.viewHi);
    }
    return cp;
}

static void g633AppendGuestBits(const std::array<uint64_t, 8> &bits,
                                std::vector<uint32_t> &pages, std::vector<uint32_t> &words)
{
    uint32_t p = 0u;
    while (p < kG630Pages)
    {
        if ((bits[p >> 6u] & (1ull << (p & 63u))) == 0ull) { ++p; continue; }
        uint32_t q = p + 1u;
        while (q < kG630Pages && (bits[q >> 6u] & (1ull << (q & 63u))) != 0ull) ++q;
        g630AppendGuestPages(p, q - p, pages, words);
        p = q;
    }
}

static bool g633PrepareSurfaceView(G633TexturePlan &p, G633ConsumerPlan &cp)
{
    if (!s_g636LinView || s_g636NoCt32View) return false; // G636: retired, see g630PrepareView
    if (cp.surface == nullptr || !cp.surface->valid) return false;
    G630Surface &s = *cp.surface;
    G633ResidencyWindow *w = g633WindowPlan(cp.windowPlan);
    if (w == nullptr) return false;
    if (w->dirtyPages == 0u) { ++g_g633BindFast; return true; }
    // One GS CT32 page is 32 rows. Publishing complete page bands makes page-residency tokens
    // exact for every compiled consumer sharing this surface.
    const int desiredLo = std::clamp(w->viewLo & ~31, 0, 511);
    const int desiredHi = std::clamp(w->viewHi | 31, desiredLo, 511);
    // G636 defect A, compiled arm: identical proof to g630PrepareView — this decode overwrites the
    // native renderer's own FBO for this fbp, so it may not run while a dirty G261 target owns any
    // page of the band.
    if (g636G261DirtyOverlaps(g636SurfaceBandRange(s.fbp, s.fbw, desiredLo, desiredHi)))
    {
        g_g636ArbCt32.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    auto packet = std::make_shared<G630ViewPacket>();
    packet->fbp = s.fbp; packet->fbw = static_cast<int>(s.fbw);
    packet->width = s.fbW; packet->height = 512;
    packet->originY = desiredLo; packet->rows = desiredHi - desiredLo + 1;
    static bool s_pageMapSubmitted = false;
    if (!s_pageMapSubmitted)
    {
        packet->pageWords.resize(64u * 32u);
        for (uint32_t y = 0u; y < 32u; ++y)
            for (uint32_t x = 0u; x < 64u; ++x)
            {
                const uint32_t word = GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u;
                if (word >= 2048u) return false;
                packet->pageWords[y * 64u + x] = word;
            }
    }
    // G636: patch the whole DECODED band, not only the sampled window — the band is what the worker
    // writes into the FBO. `_NO_BANDPATCH` restores the sample-scoped form verbatim.
    if (s_g636NoBandPatch)
        g633AppendGuestBits(w->sampleBits, packet->patchPages, packet->patchWords);
    else
        g636AppendSurfaceBandPages(s.fbp, s.fbw, desiredLo, desiredHi,
                                   packet->patchPages, packet->patchWords);
    if (!g630_backend_prepare_ct32_view(packet)) return false;
    s_pageMapSubmitted = true;
    g630CommitSyncedPages(packet->patchPages);
    G630Pending pending; pending.view = true;
    g_g630Pending.push_back(std::move(pending));
    const bool sameView = s.viewSeq == s.seq;
    s.viewSeq = s.seq;
    s.viewLo = sameView ? std::min(s.viewLo, desiredLo) : desiredLo;
    s.viewHi = sameView ? std::max(s.viewHi, desiredHi) : desiredHi;
    s.viewGlobalGen = g_g149VramGen.load(std::memory_order_relaxed);
    g633PublishSurfaceBits(s, w->sampleBits);
    s.viewGeneration = g633BitsSubset(s.pageBits, s.residentBits) ? s.generation : 0u;
    const bool samePlanView = p.ct32Surface == &s && p.ct32ShapeGeneration == s.shapeGeneration;
    p.ct32Surface = &s;
    p.ct32ShapeGeneration = s.shapeGeneration;
    p.ct32PreparedGeneration = p.textureGeneration;
    p.ct32ViewLo = samePlanView ? std::min(p.ct32ViewLo, desiredLo) : desiredLo;
    p.ct32ViewHi = samePlanView ? std::max(p.ct32ViewHi, desiredHi) : desiredHi;
    g_g630Views.fetch_add(1u, std::memory_order_relaxed);
    ++g_g633BindBuilds;
    return true;
}

static void g633VerifyBindDecision(uint32_t dstFbp, const G144Entry &e,
                                   const G178EntryState &st, bool alreadyBound,
                                   const G633ConsumerPlan *cp, uint32_t compiledMask)
{
    if (!s_g633Verify) return;
    G630Surface *legacySurface = nullptr;
    int legacyLo = 0, legacyHi = -1;
    G144BlockRange legacyRange{};
    const uint32_t legacyMask = g630BindRejectMask(
        dstFbp, e, st, alreadyBound, &legacySurface, &legacyLo, &legacyHi, &legacyRange);
    ++g_g633VerifyBind;
    const G633TexturePlan *tp = g633TexturePlanConst(st.residencyPlan);
    const bool bad = legacyMask != compiledMask ||
        (legacyMask == 0u && (cp == nullptr || cp->surface != legacySurface ||
         cp->viewLo != legacyLo || cp->viewHi != legacyHi || tp == nullptr ||
         tp->texRange.valid != legacyRange.valid || tp->texRange.first != legacyRange.first ||
         tp->texRange.last != legacyRange.last));
    if (bad) ++g_g633VerifyBad;
}

static uint32_t g633TryBind(uint32_t dstFbp, const G144Entry &e,
                            const G178EntryState &st, bool alreadyBound, uint32_t *outMask)
{
    uint32_t m = st.residencyBindMask;
    if (!g630DomainOn()) m |= kG632BindOff;
    if (alreadyBound) m |= kG632BindAlready;
    if (!e.prim.tme) m |= kG632BindNoTme;
    if (st.skip) m |= kG632BindSkip;
    if (m != 0u)
    {
        g633VerifyBindDecision(dstFbp, e, st, alreadyBound, nullptr, m);
        if (outMask) *outMask = m;
        ++g_g633BindRejects;
        return 0u;
    }
    G633ConsumerPlan &cp = g633ResolveConsumer(e, st);
    m = cp.mask;
    g633VerifyBindDecision(dstFbp, e, st, alreadyBound, &cp, m);
    if (m != 0u || cp.surface == nullptr)
    {
        if (outMask) *outMask = m;
        ++g_g633BindRejects;
        return 0u;
    }
    G633TexturePlan *tp = g633TexturePlan(st.residencyPlan);
    if (tp == nullptr || !g633PrepareSurfaceView(*tp, cp))
    {
        if (outMask) *outMask = kG632BindView;
        return 0u;
    }
    G633ResidencyWindow *w = g633WindowPlan(cp.windowPlan);
    if (w != nullptr) w->requiredPass = g_g633Pass;
    if (outMask) *outMask = 0u;
    g_g630DirectBinds.fetch_add(1u, std::memory_order_relaxed);
    return cp.surface->fbp;
}

static bool g633BuildT8View(uint16_t id, G633TexturePlan &p)
{
    (void)g633MaterializeClutPlan(id, kG261MatTexAlias);
    if (!g630DomainOn()) return false;
    // G636: the compiled arm's copy of the same arbitration (see g630TryT8View).
    if (s_g636NoT8View ||
        g636G261DirtyOverlaps(p.texRange) || g636G261DirtyOverlaps(p.clutRange))
    {
        g_g636ArbT8.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (p.t8Resident && p.t8BuiltGeneration == p.contentGeneration)
    {
        ++g_g633T8Fast;
        return true;
    }
    const uint64_t buildGeneration = p.contentGeneration;
    auto packet = std::make_shared<G630T8Packet>();
    packet->texKey = p.viewKey;
    packet->tbp = p.desc.tbp;
    packet->tbw = std::max(1u, p.desc.tbw >> 1u);
    packet->width = p.texW; packet->height = p.texH;
    static bool s_permSubmitted = false;
    const G618P8Page &p8 = g618P8Page();
    if (!p8.ok) return false;
    if (!s_permSubmitted) packet->mapWords.assign(p8.perm.begin(), p8.perm.end());
    packet->clut.resize(256u);
    for (uint32_t ci = 0u; ci < 256u; ++ci)
        packet->clut[ci] = g178LookupClutFixed(
            g_g630Vram, p.texa, p.texclut, static_cast<uint8_t>(ci), p.desc.cbp,
            p.desc.cpsm, p.desc.csm, p.desc.csa, p.desc.psm);
    g633AppendGuestBits(p.texBits, packet->patchPages, packet->patchWords);
    std::vector<uint32_t> clutPages, clutWords;
    g633AppendGuestBits(p.clutBits, clutPages, clutWords);
    for (size_t i = 0u; i < clutPages.size(); ++i)
    {
        if (std::find(packet->patchPages.begin(), packet->patchPages.end(), clutPages[i]) !=
            packet->patchPages.end()) continue;
        packet->patchPages.push_back(clutPages[i]);
        packet->patchWords.insert(packet->patchWords.end(),
            clutWords.begin() + i * kG630WordsPerPage,
            clutWords.begin() + (i + 1u) * kG630WordsPerPage);
    }
    G630Pending pending; pending.t8 = packet; pending.view = true;
    if (!g630_backend_prepare_t8_view(packet))
    {
        g630RecoverFailure(pending);
        return false;
    }
    s_permSubmitted = true;
    g630CommitSyncedPages(packet->patchPages);
    g_g630Pending.push_back(std::move(pending));
    p.t8Resident = true;
    p.t8BuiltGeneration = buildGeneration;
    g_g630T8Views.fetch_add(1u, std::memory_order_relaxed);
    g_g630T8Pixels.fetch_add(static_cast<uint64_t>(p.texW) * p.texH,
                             std::memory_order_relaxed);
    ++g_g633T8Builds;
    return true;
}

static uint64_t g633TryT8View(const G144Entry &e, const G178EntryState &st, uint32_t *outMask)
{
    G633TexturePlan *p = g633TexturePlan(st.residencyPlan);
    if (p == nullptr) return 0u;
    uint32_t m = st.residencyT8Mask;
    if (!g630DomainOn()) m |= kG632T8Off;
    if (!g_g630Initialized) m |= kG632T8Uninit;
    if (!e.prim.tme) m |= kG632T8NoTme;
    if (st.skip) m |= kG632T8Skip;
    if (m == 0u)
    {
        if (!p->texRange.valid) m |= kG632T8Range;
        if (p->texDirty == 0u) m |= kG632T8NoAuthority;
        if (!p->clutRange.valid) m |= kG632T8Clut;
    }
    if (s_g633Verify)
    {
        G144BlockRange legacyTex{}, legacyClut{};
        const uint32_t legacy = g630T8RejectMask(e, st, &legacyTex, &legacyClut);
        ++g_g633VerifyT8;
        const uint32_t cheap = kG632T8Off | kG632T8Uninit | kG632T8NoTme |
                               kG632T8Skip | kG632T8Psm | kG632T8NoKey | kG632T8Dim;
        const bool compareRanges = (legacy & cheap) == 0u;
        if (legacy != m || (compareRanges &&
            (legacyTex.valid != p->texRange.valid || legacyClut.valid != p->clutRange.valid ||
             (legacyTex.valid && (legacyTex.first != p->texRange.first ||
                                  legacyTex.last != p->texRange.last)) ||
             (legacyClut.valid && (legacyClut.first != p->clutRange.first ||
                                   legacyClut.last != p->clutRange.last)))))
            ++g_g633VerifyBad;
    }
    if (m != 0u)
    {
        if (outMask) *outMask = m;
        ++g_g633T8Rejects;
        return 0u;
    }
    if (outMask) *outMask = kG632T8Build;
    if (!g633BuildT8View(static_cast<uint16_t>(st.residencyPlan), *p)) return 0u;
    p->requiredPass = g_g633Pass;
    if (outMask) *outMask = 0u;
    return p->viewKey;
}

static void g633BeginTexturePass()
{
    ++g_g633Pass;
    if (g_g633Pass == 0u) ++g_g633Pass;
    g_g633FinalSurfaces.clear();
    g_g633FinalWindows.clear();
    g_g633FinalT8.clear();
    // A bounded fallback plan can still enter the frozen path; keep its per-pass set exact.
    g_g630T8Built.clear();
}

static void g633FinalizeTexturePass()
{
    if (!g633PlansOn()) return;
    for (uint32_t id : g_g633FinalWindows)
    {
        G633ResidencyWindow *w = g633WindowPlan(id);
        if (w == nullptr || w->requiredPass != g_g633Pass ||
            w->finalQueuedPass != g_g633Pass || w->surface == nullptr ||
            !w->surface->valid || w->surface->shapeGeneration != w->surfaceGeneration)
            continue;
        if (w->dirtyPages == 0u) continue;
        G630Surface &s = *w->surface;
        const int desiredLo = std::clamp(w->viewLo & ~31, 0, 511);
        const int desiredHi = std::clamp(w->viewHi | 31, desiredLo, 511);
        auto packet = std::make_shared<G630ViewPacket>();
        packet->fbp = s.fbp; packet->fbw = static_cast<int>(s.fbw);
        packet->width = s.fbW; packet->height = 512;
        packet->originY = desiredLo; packet->rows = desiredHi - desiredLo + 1;
        g633AppendGuestBits(w->sampleBits, packet->patchPages, packet->patchWords);
        if (g630_backend_prepare_ct32_view(packet))
        {
            g630CommitSyncedPages(packet->patchPages);
            G630Pending pending; pending.view = true;
            g_g630Pending.push_back(std::move(pending));
            g633PublishSurfaceBits(s, w->sampleBits);
            ++g_g633FinalRepairs;
        }
    }
    for (uint16_t id : g_g633FinalT8)
    {
        G633TexturePlan *p = g633TexturePlan(id);
        if (p != nullptr && g633BuildT8View(id, *p)) ++g_g633FinalRepairs;
    }
}

static void g633Report()
{
    if (!s_g633Stat && !s_g633Verify) return;
    std::fprintf(stderr,
        "[G633:plans] tex=%zu lookup=%llu hit=%llu compile=%llu refused=%llu "
        "consumer=%llu hit=%llu miss=%llu reval=%llu\n",
        g_g633TexturePlans.size(),
        (unsigned long long)g_g633TextureLookups, (unsigned long long)g_g633TextureHits,
        (unsigned long long)g_g633TextureCompiles, (unsigned long long)g_g633TextureRefused,
        (unsigned long long)g_g633ConsumerProbes, (unsigned long long)g_g633ConsumerHits,
        (unsigned long long)g_g633ConsumerMisses, (unsigned long long)g_g633ConsumerRevalidates);
    std::fprintf(stderr,
        "[G633:resident] bind(rej=%llu fast=%llu build=%llu) "
        "t8(rej=%llu fast=%llu build=%llu) mat(fast=%llu break=%llu) "
        "event(auth=%llu content=%llu surface=%llu consumer=%llu/%llu final=%llu "
        "guest(stage=%llu commit=%llu rawcancel=%llu))\n",
        (unsigned long long)g_g633BindRejects, (unsigned long long)g_g633BindFast,
        (unsigned long long)g_g633BindBuilds, (unsigned long long)g_g633T8Rejects,
        (unsigned long long)g_g633T8Fast, (unsigned long long)g_g633T8Builds,
        (unsigned long long)g_g633MaterializeFast, (unsigned long long)g_g633MaterializeBreaks,
        (unsigned long long)g_g633AuthorityEvents, (unsigned long long)g_g633ContentEvents,
        (unsigned long long)g_g633SurfaceEvents,
        (unsigned long long)g_g633ConsumerDirtyEvents,
        (unsigned long long)g_g633ConsumerPublishes,
        (unsigned long long)g_g633FinalRepairs,
        (unsigned long long)g_g633GuestStages, (unsigned long long)g_g633GuestCommits,
        (unsigned long long)g_g633GuestRawCancels);
    if (s_g633Stat)
    {
        std::fprintf(stderr, "[G633:guestsrc]");
        for (uint32_t source = 0u; source < g_g633GuestStagesBySource.size(); ++source)
            if (g_g633GuestStagesBySource[source] != 0u)
                std::fprintf(stderr, " %u=%llu", source,
                             (unsigned long long)g_g633GuestStagesBySource[source]);
        std::fprintf(stderr, "\n");
    }
    if (s_g633Verify)
        std::fprintf(stderr, "[G633:verify] bind=%llu t8=%llu authority=%llu bad=%llu\n",
            (unsigned long long)g_g633VerifyBind, (unsigned long long)g_g633VerifyT8,
            (unsigned long long)g_g633VerifyAuthority, (unsigned long long)g_g633VerifyBad);
}
