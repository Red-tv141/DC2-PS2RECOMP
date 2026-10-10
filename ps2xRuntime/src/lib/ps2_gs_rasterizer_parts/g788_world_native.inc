// ===== G788 — WHOLE-WORLD NATIVE FixMDT, rasterizer-TU side (plans/phase-G788-ledger.md) =====
// Included right after g787_native_composite.inc at global scope (needs G144Entry).
//
// PARSE THREAD (g788OnTagMarker): a native face marker arrives at the face's exact stream position,
// after every state DIRECT of its legacy node (renderInfo, material TEX0/TEX1/CLAMP, PRMODE). The
// face's FIRST emitted primitive is computed by the per-face CPU reference and kicked through the
// GS front door with the register writes the VU PATH1 output would make (PRIM, RGBAQ(+Q), ST,
// XYZF2), so cull / deferral / capture are the legacy ones. The kick that reaches the G144 defer
// push is converted IN PLACE into ONE macro entry covering the whole face (bbox rows = scissor);
// a kick taken inline leaves the face as exact legacy primitives (every remaining one is kicked).
// The GS latches (PRIM/RGBAQ/ST/XYZ queue) now hold a value the legacy stream would not have left:
// marked G778-dirty with the G788 owner bit (counted, not fatal, if a later kick consumes one).
// EXECUTOR: the vertex build emits one placeholder draw per macro (3 zero vertices in the batch
// VBO) carrying the job; the batch's job table is built after the build; the GL worker transforms
// the face on the GPU and draws it in that placeholder's slot of the ordered draw list. Anything the
// GPU path cannot take (an FBO-bound texture source, FST) and every CPU replay expands the macro
// back into the exact legacy triangles from the CPU reference.

int g_g788Mode = [] {
    // Static initialization: plain getenv only.
    // G791 release: the accepted G788-AA world path (plans/g788-final-report.md) is DEFAULT ON; it used to
    // need DC2_G788_WORLD=native. Rollback DC2_G788_WORLD=off (or 0).
    const char *e = std::getenv("DC2_G788_WORLD");
    if (e == nullptr || *e == '\0')
        return 3;
    if (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0)
        return 0;
    if (std::strcmp(e, "census") == 0 || std::strcmp(e, "1") == 0)
        return 1;
    if (std::strcmp(e, "drop") == 0 || std::strcmp(e, "2") == 0)
        return 2;
    if (std::strcmp(e, "native") == 0 || std::strcmp(e, "3") == 0)
        return 3;
    if (std::strcmp(e, "verify") == 0 || std::strcmp(e, "4") == 0)
        return 4;
    if (std::strcmp(e, "dropall") == 0 || std::strcmp(e, "5") == 0)
        return 5;
    return 0;
}();
G788Job g_g788Ring[kG788Ring];
G788Counters g_g788C;
std::atomic<int> g_g788VerifyArmed{0};
G788KickCounters g_g788K;

constexpr float kG788MacroQ = -78801.0f;
struct G788PushHook
{
    uint32_t mode = 0, consumed = 0, seq = 0; // 1: convert; 2: verify; 3: capture-only synthetic primer
    size_t cursor = 0, cursorB = 0;           // verify: hypothesis A (draw flags) / B (clipping forced on)
    bool faceBad = false, faceBadB = false;
    uint32_t clipMode = 3;
};
static G788PushHook g_g788Push;
static std::shared_ptr<const G788DrawRec> g_g788VerifyRec;
static uint32_t g_g788VerifyFace = 0;
// VERIFY only: producer face ID is copied into each ordered MTVU kick. Snapshot from the
// worker's persistent RAM after write replay; never inspect the EE shadow as VU authority.
static thread_local uint32_t t_g788VuFace = 0;
// Publication and DMA staging both run on the EE owner. Consume at the original REF tag,
// before transport publication; asynchronous VIF/GS consumers receive immutable marker bytes.
struct G788WatchedRef { uint64_t tag; uint32_t seq; };
static thread_local std::unordered_map<uint32_t, G788WatchedRef> t_g788Refs;
static thread_local uint32_t t_g788RefFrame = ~0u;
void dc2_g788_watchRef(uint32_t frame, uint32_t address, uint64_t tag, uint32_t seq)
{
    if (frame != t_g788RefFrame)
    {
        t_g788Refs.clear();
        t_g788RefFrame = frame;
    }
    t_g788Refs[address] = {tag, seq};
}
uint32_t dc2_g788_takeRef(uint32_t address, uint64_t tag)
{
    const auto found = t_g788Refs.find(address);
    if (found == t_g788Refs.end()) return 0u;
    const uint32_t seq = found->second.tag == tag ? found->second.seq : 0u;
    t_g788Refs.erase(found);
    return seq;
}
struct G788EconRef { uint64_t original,committed;uint32_t seq,frame; };
static thread_local std::unordered_map<uint32_t,G788EconRef> t_g788EconRefs;
static thread_local uint32_t t_g788EconRefFrame=0;
void dc2_g788_econ_watch_ref(uint32_t frame,uint32_t address,uint64_t original,uint64_t committed,uint32_t seq)
{
    if(frame!=t_g788EconRefFrame) {
        dc2_g788_econ::event(dc2_g788_econ::NativeRefsUnobserved,t_g788EconRefs.size());
        t_g788EconRefs.clear();t_g788EconRefFrame=frame;
    }
    t_g788EconRefs[address]={original,committed,seq,frame};
    dc2_g788_econ::event(dc2_g788_econ::NativeRefsPublished);
}
void dc2_g788_econ_observe_ref(uint32_t address,uint64_t tag)
{
    const auto it=t_g788EconRefs.find(address);
    if(it==t_g788EconRefs.end())return;
    const auto ref=it->second;t_g788EconRefs.erase(it);
    if(tag==ref.committed)dc2_g788_econ::event(dc2_g788_econ::NativeRefsConsumed);
    else {
        dc2_g788_econ::event(tag==ref.original?dc2_g788_econ::AcceptedLegacyRefs:dc2_g788_econ::NativeRefsMismatch);
        std::fprintf(stderr,"[G788:wire] FATAL accepted face transport changed frame=%u seq=%u address=%x\n",ref.frame,ref.seq,address);
        std::abort();
    }
}
struct G788VuSnapshot
{
    std::atomic<uint32_t> seq{0};
    uint32_t pc = 0, resume = 0;
    float vf[2][4]{};
    uint8_t memory[64 * 16]{};
};
static std::unique_ptr<G788VuSnapshot[]> g_g788VuSnapshots =
    g_g788Mode == 4 ? std::make_unique<G788VuSnapshot[]>(kG788Ring) : nullptr;
void dc2_g788_vifMarker(uint64_t tagHi)
{
    if ((tagHi >> 48) == kG788MarkerMagic && ((tagHi >> 47) & 1u))
        t_g788VuFace = ((tagHi >> 46) & 1u) ? 0u : uint32_t(tagHi);
}
uint32_t dc2_g788_vuFace() { return t_g788VuFace; }
void dc2_g788_captureVu(uint32_t seq, const uint8_t *data, const float (*vf)[4], uint32_t pc, bool resume)
{
    if (!g_g788VuSnapshots || !data || !vf)
        return;
    G788VuSnapshot &snapshot = g_g788VuSnapshots[seq & (kG788Ring - 1u)];
    if (snapshot.seq.load(std::memory_order_acquire) == seq)
        return; // first kick of the face; continuations cannot replace its initial setup
    snapshot.seq.store(0u, std::memory_order_release);
    snapshot.pc = pc;
    snapshot.resume = resume ? 1u : 0u;
    std::memcpy(snapshot.vf, vf[15], sizeof(snapshot.vf));
    std::memcpy(snapshot.memory, data, sizeof(snapshot.memory));
    snapshot.seq.store(seq, std::memory_order_release);
}
static void g788VerifyVuState()
{
    const auto &h = g_g788Push;
    static uint32_t dumps[4]{}, logs = 0;
    static FILE *output = nullptr;
    if (!g_g788VuSnapshots || !g_g788VerifyRec)
        return;
    const auto &snapshot = g_g788VuSnapshots[h.seq & (kG788Ring - 1u)];
    if (snapshot.seq.load(std::memory_order_acquire) != h.seq)
    {
        if (logs++ < 12u) std::fprintf(stderr, "[G788:vu-state] seq=%u snapshotMissing=1\n", h.seq);
        return;
    }
    uint32_t flags;
    std::memcpy(&flags, snapshot.memory + 38u * 16u, 4u);
    const uint32_t engine = g_g788VerifyRec->draw.renderFlags;
    if (flags != engine && logs++ < 24u)
        std::fprintf(stderr, "[G788:vu-state] seq=%u mode=%u engineFlags=%x vuFlags=%x pc=%x resume=%u\n",
                     h.seq, h.clipMode, engine, flags, snapshot.pc, snapshot.resume);
    const uint32_t mode = h.clipMode & 3u;
    if (!(h.faceBad || flags != engine) && dumps[mode] >= 20u)
        return;
    if (dumps[mode]++ >= 160u)
        return;
    if (!output) output = std::fopen("logs/g788/vu_state.bin", "wb");
    if (!output) return;
    const uint32_t header[16] = {0x53563837u, mode, h.seq, g_g788VerifyFace, engine, flags,
                                h.faceBad ? 1u : 0u, g_g788VerifyRec->frame, snapshot.pc, snapshot.resume,
                                64u, 0, 0, 0, 0, 0};
    std::fwrite(header, sizeof(header), 1, output);
    std::fwrite(snapshot.vf, sizeof(snapshot.vf), 1, output);
    std::fwrite(snapshot.memory, sizeof(snapshot.memory), 1, output);
    std::fflush(output);
}
static std::vector<dc2_native::ReferencePrimitive> g_g788VerifyPrims, g_g788VerifyPrimsB; // parse thread only
// VERIFY: the legacy front door G458-culls a primitive whose bbox misses the scissor (no push). The
// expected push sequence is the reference minus exactly those (same predicate, compare form), under
// the context of the face's pushes (or the last verified push when a face pushes nothing).
static GSContext g_g788VerifyCtx{};
static bool g_g788VerifyHaveCtx = false;
static bool g788G458On()
{
    static const bool on = [] {
        if (std::getenv("DC2_G458_NO_CULL_CLIPPED") != nullptr)
            return false;
        const char *e = std::getenv("DC2_G458_CULL_CLIPPED");
        return !(e != nullptr && (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0));
    }();
    return on;
}
// Build E: the oracle compares at drawPrimitive ENTRY (every primitive the legacy face kicks, before
// the G458 cull and the defer/inline decision), so no culled-reference adjustment is needed.
static constexpr bool kG788VerifyAtKick = true;
static bool g788Culled(const dc2_native::ReferencePrimitive &p, const GSContext &c, bool countAtEntry = false)
{
    if ((!countAtEntry && kG788VerifyAtKick) || !g788G458On())
        return false;
    const float ofx = static_cast<float>(c.xyoffset.ofx >> 4), ofy = static_cast<float>(c.xyoffset.ofy >> 4);
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    for (const auto &r : p.vertices)
    {
        const float x = static_cast<float>(r.x & 0xFFFFu) / 16.0f, y = static_cast<float>(r.y & 0xFFFFu) / 16.0f;
        mnx = std::min(mnx, x); mxx = std::max(mxx, x); mny = std::min(mny, y); mxy = std::max(mxy, y);
    }
    return (mxx - ofx) <= static_cast<float>(c.scissor.x0) - 1.0f || (mnx - ofx) >= static_cast<float>(c.scissor.x1) + 1.0f ||
           (mxy - ofy) <= static_cast<float>(c.scissor.y0) - 1.0f || (mny - ofy) >= static_cast<float>(c.scissor.y1) + 1.0f;
}
static size_t g788Unculled(const std::vector<dc2_native::ReferencePrimitive> &v, size_t from)
{
    size_t n = 0;
    for (size_t i = from; i < v.size(); ++i)
        if (!g_g788VerifyHaveCtx || !g788Culled(v[i], g_g788VerifyCtx))
            ++n;
    return n;
}
static uint32_t g_g788VerifyLogs = 0;
// VERIFY trace of the current face: every legacy push (type, first vertex) for a full-sequence dump.
struct G788PushTrace { int type; float x, y; uint32_t matched; GSVertex v[3]; };
// VERIFY face fixture (DC2_G788_VFIX=K): the first K bad and K/4 good faces per clip mode, self-contained
// (draw inputs + face corners + every legacy kick) -> logs/g788/vfix.bin, for offline reference work.
static uint32_t g788VfixN()
{
    static const uint32_t v = [] { const char *e = std::getenv("DC2_G788_VFIX"); return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u; }();
    return v;
}
static std::vector<G788PushTrace> g_g788VerifyTrace;
static uint32_t g_g788VerifyDumps[4] = {0, 0, 0, 0};
static void g788VerifyFixture()
{
    G788PushHook &h = g_g788Push;
    static uint32_t bad[4] = {0, 0, 0, 0}, good[4] = {0, 0, 0, 0};
    const uint32_t m = h.clipMode & 3u, K = g788VfixN();
    if (!K || !g_g788VerifyRec)
        return;
    if (h.faceBad ? bad[m]++ >= K : good[m]++ >= K / 4u + 1u)
        return;
    static FILE *f = nullptr;
    if (!f)
        f = std::fopen("logs/g788/vfix.bin", "wb");
    if (!f)
        return;
    const G788DrawRec &r = *g_g788VerifyRec;
    const auto &d = r.draw;
    const auto &face = d.geometry->faces[g_g788VerifyFace];
    const uint32_t hdr[16] = {0x58463837u, m, h.seq, g_g788VerifyFace, h.faceBad ? 1u : 0u, uint32_t(face.indices.size()),
                              uint32_t(g_g788VerifyTrace.size()), d.renderFlags, face.topology, face.batchCorners, face.material,
                              r.frame, 0, 0, 0, 0};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    std::fwrite(d.world.data(), 64, 1, f);
    std::fwrite(d.cameraScreen.data(), 64, 1, f);
    std::fwrite(d.cameraClip.data(), 64, 1, f);
    std::fwrite(d.cameraViewport.data(), 64, 1, f);
    std::fwrite(d.lightDirections.data(), 48, 1, f);
    std::fwrite(d.lightColours.data(), 64, 1, f);
    std::fwrite(d.ambient.data(), 16, 1, f);
    std::fwrite(d.fog.data(), 16, 1, f);
    std::fwrite(d.materials[face.material].colour.data(), 16, 1, f);
    for (const uint32_t vi : face.indices)
    {
        const auto &v = d.geometry->vertices[vi];
        std::fwrite(v.position.data(), 16, 1, f);
        std::fwrite(v.normal.data(), 16, 1, f);
        std::fwrite(v.uv.data(), 16, 1, f);
        std::fwrite(v.colour.data(), 16, 1, f);
    }
    for (const auto &t : g_g788VerifyTrace)
    {
        const uint32_t type = uint32_t(t.type);
        std::fwrite(&type, 4, 1, f);
        for (const auto &g : t.v)
        {
            const float fl[6] = {g.x, g.y, g.z, g.q, g.s, g.t};
            std::fwrite(fl, sizeof(fl), 1, f);
            const uint8_t c[8] = {g.r, g.g, g.b, g.a, g.fog, 0, 0, 0};
            std::fwrite(c, sizeof(c), 1, f);
        }
    }
    std::fflush(f);
}
static void g788VerifyDump()
{
    g788VerifyFixture();
    G788PushHook &h = g_g788Push;
    if (!h.faceBad || g_g788VerifyDumps[h.clipMode & 3u] >= 6u)
        return;
    ++g_g788VerifyDumps[h.clipMode & 3u];
    std::fprintf(stderr, "[G788:vdump] clip=%u seq=%u ref=%zu pushes=%zu\n", h.clipMode, h.seq, g_g788VerifyPrims.size(), g_g788VerifyTrace.size());
    for (size_t i = 0; i < g_g788VerifyPrims.size() && i < 64u; ++i)
    {
        const auto &p = g_g788VerifyPrims[i];
        std::fprintf(stderr, "[G788:vdump]  ref %zu batch=%u clipped=%u corner=%u v0=(%.4f,%.4f)\n", i, p.batch,
                     unsigned(p.clipped), p.sourceCorner, (p.vertices[0].x & 0xFFFFu) / 16.0, (p.vertices[0].y & 0xFFFFu) / 16.0);
    }
    for (size_t i = 0; i < g_g788VerifyTrace.size() && i < 64u; ++i)
    {
        const auto &t = g_g788VerifyTrace[i];
        std::fprintf(stderr, "[G788:vdump]  got %zu type=%d match=%u v0=(%.4f,%.4f)\n", i, t.type, t.matched, double(t.x),
                     double(t.y));
    }
}

static inline uint32_t g788FloatBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
static void g788KickPrim(GS *gs, const dc2_native::ReferencePrimitive &p)
{
    for (const auto &v : p.vertices)
    {
        gs->writeRegister(GS_REG_RGBAQ, static_cast<uint64_t>(v.rgba[0]) | (static_cast<uint64_t>(v.rgba[1]) << 8) |
                                            (static_cast<uint64_t>(v.rgba[2]) << 16) |
                                            (static_cast<uint64_t>(v.rgba[3]) << 24) |
                                            (static_cast<uint64_t>(g788FloatBits(v.q)) << 32));
        gs->writeRegister(GS_REG_ST, static_cast<uint64_t>(g788FloatBits(v.s)) |
                                         (static_cast<uint64_t>(g788FloatBits(v.t)) << 32));
        gs->writeRegister(GS_REG_XYZF2, static_cast<uint64_t>(v.x & 0xFFFFu) |
                                            (static_cast<uint64_t>(v.y & 0xFFFFu) << 16) |
                                            (static_cast<uint64_t>(v.z & 0xFFFFFFu) << 32) |
                                            (static_cast<uint64_t>(v.fog) << 56));
    }
}
static void g788MarkLatchesDirty()
{
#if defined(PS2X_G777_NATIVE_GL)
    dc2_g778::gsDirty.store(31u | 0x80000000u, std::memory_order_relaxed);
#endif
}

// Admission uses a small synthetic triangle entirely inside the active scissor. Its only
// possible output is a G144 macro: mode3 makes drawPrimitive refuse every inline path, and
// bypasses compact strip capture. The GPU replaces every synthetic vertex before raster.
// Unsupported state falls back to the independently proven reference without drawing this
// triangle. No source vertex is transformed, and no worker call/wait is introduced here.
bool GS::primeNativeWorldFace(uint32_t seq)
{
    const auto &ctx = activeContext();
    const uint32_t x = ((ctx.xyoffset.ofx >> 4) + ctx.scissor.x0 + 1u) * 16u;
    const uint32_t y = ((ctx.xyoffset.ofy >> 4) + ctx.scissor.y0 + 1u) * 16u;
    if (ctx.scissor.x1 <= ctx.scissor.x0 + 1u || ctx.scissor.y1 <= ctx.scissor.y0 + 1u ||
        x + 16u > 65535u || y + 16u > 65535u)
        return false;
    dc2_native::ReferencePrimitive p{};
    for (auto &v : p.vertices)
    {
        v.x = x; v.y = y; v.z = 1u;
        v.q = 1.0f; v.s = v.t = 0.0f;
        v.rgba = {128, 128, 128, 128}; v.fog = 128;
    }
    p.vertices[1].x += 16u;
    p.vertices[2].y += 16u;
    auto &h = g_g788Push;
    h.mode = 3u; h.consumed = 0u; h.seq = seq;
    g788KickPrim(this, p);
    h.mode = 0u;
    return h.consumed != 0u;
}

// GS parse thread, processGIFPacket: a payload-less tag carrying a face job.
void g788OnTagMarker(GS *gs, uint64_t tagHi)
{
    dc2_g788_econ::Scope econ(dc2_g788_econ::ParseMarker);
    g_g788C.markers.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t seq = static_cast<uint32_t>(tagHi);
    if (seq == kG788DropJob)
    {
        g_g788C.dropMarkers.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    if ((tagHi >> 47) & 1ull)
    {
        // VERIFY: host DMA staging brackets the unchanged legacy face REF payload.
        G788PushHook &v = g_g788Push;
        if ((tagHi >> 46) & 1ull)
        {
            // END: every PATH1 output of the face has been pushed (the face packet ends with FLUSHA).
            if (v.mode != 2u || v.seq != seq)
                return;
            if (const size_t miss = g788Unculled(g_g788VerifyPrims, v.cursor))
            {
                g_g788C.vMissing.fetch_add(miss, std::memory_order_relaxed);
                v.faceBad = true;
            }
            if (g788Unculled(g_g788VerifyPrimsB, v.cursorB))
                v.faceBadB = true;
            const uint32_t m = v.clipMode & 3u;
            g_g788C.vmFaces[m].fetch_add(1u, std::memory_order_relaxed);
            g_g788C.vmExtraA[m].fetch_add(g788Unculled(g_g788VerifyPrims, v.cursor), std::memory_order_relaxed);
            g_g788C.vmExtraB[m].fetch_add(g788Unculled(g_g788VerifyPrimsB, v.cursorB), std::memory_order_relaxed);
            if (v.faceBad)
                g_g788C.vmBadA[m].fetch_add(1u, std::memory_order_relaxed);
            if (v.faceBadB)
                g_g788C.vmBadB[m].fetch_add(1u, std::memory_order_relaxed);
            g788VerifyVuState();
            g788VerifyDump();
            v.mode = 0u;
            g_g788VerifyArmed.store(2, std::memory_order_relaxed);
            g_g788Ring[seq & (kG788Ring - 1u)].verifyConsumed.store(seq, std::memory_order_release);
            return;
        }
        v.mode = 0u;
        v.faceBad = v.faceBadB = false;
        v.cursor = v.cursorB = 0;
        g_g788VerifyTrace.clear();
        std::shared_ptr<const G788DrawRec> vrec;
        uint32_t vface = 0;
        if (!g788Lookup(seq, vrec, vface))
        {
            g_g788C.orphans.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        g_g788VerifyPrims.clear();
        g_g788VerifyPrimsB.clear();
        if (!g788FacePrims(*vrec, vface, g_g788VerifyPrims, false) ||
            !g788FacePrims(*vrec, vface, g_g788VerifyPrimsB, false, 3))
        {
            g_g788C.refPrimFail.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        g_g788C.vFaces.fetch_add(1u, std::memory_order_relaxed);
        g_g788VerifyRec = vrec;
        g_g788VerifyFace = vface;
        v.mode = 2u;
        g_g788VerifyArmed.store(1, std::memory_order_relaxed);
        v.seq = seq;
        v.clipMode = vrec->clipMode;
        const uint32_t m = v.clipMode & 3u;
        const auto &vf = vrec->draw.geometry->faces[vface];
        g_g788C.vmInput[m].fetch_add(vf.topology == 3 ? vf.indices.size() / 3u : vf.indices.size() - 2u,
                                    std::memory_order_relaxed);
        g_g788C.vmCandidateA[m].fetch_add(g_g788VerifyPrims.size(), std::memory_order_relaxed);
        g_g788C.vmCandidateB[m].fetch_add(g_g788VerifyPrimsB.size(), std::memory_order_relaxed);
        for (const auto &p : g_g788VerifyPrims)
            if (p.clipped) g_g788C.vmFansA[m].fetch_add(1u, std::memory_order_relaxed);
        for (const auto &p : g_g788VerifyPrimsB)
            if (p.clipped) g_g788C.vmFansB[m].fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    std::shared_ptr<const G788DrawRec> rec;
    uint32_t face = 0;
    if (!g788Lookup(seq, rec, face))
    {
        g_g788C.orphans.fetch_add(1u, std::memory_order_relaxed); // nothing to draw: VU never ran
        return;
    }
    // G791 release: DEFAULT ON (G788-AA setting); rollback DC2_G788_PRIMER=0.
    static const bool primer = [] { const char *e = std::getenv("DC2_G788_PRIMER"); return !e || std::atoi(e); }();
    if (primer && rec->clipMode == 0u)
    {
        gs->writeRegister(GS_REG_PRIM, 0x5Bu);
        if (gs->primeNativeWorldFace(seq))
        {
            g_g788C.primerMacros.fetch_add(1u, std::memory_order_relaxed);
            g_g788C.macros.fetch_add(1u, std::memory_order_relaxed);
            g788MarkLatchesDirty();
            return;
        }
        g_g788C.primerDeclined.fetch_add(1u, std::memory_order_relaxed);
    }
    g_g788C.primerReference.fetch_add(1u, std::memory_order_relaxed);
    static thread_local std::vector<dc2_native::ReferencePrimitive> prims;
    prims.clear();
    if (!g788FacePrims(*rec, face, prims, true))
    {
        g_g788C.refPrimFail.fetch_add(1u, std::memory_order_relaxed);
        g788MarkLatchesDirty();
        return;
    }
    if (prims.empty())
    {
        g_g788C.emptyFaces.fetch_add(1u, std::memory_order_relaxed);
        g788MarkLatchesDirty();
        return;
    }
    G788PushHook &h = g_g788Push;
    h.mode = 1u;
    h.consumed = 0u;
    h.seq = seq;
    gs->writeRegister(GS_REG_PRIM, 0x5Bu); // TRIANGLE | IIP | TME | ABE: the VU list tag's PRIM
    g788KickPrim(gs, prims[0]);
    if (!h.consumed)
    {
        // The first primitive was not deferred. Deferral is a function of the (face-constant) GS
        // state, so either every primitive of the face is taken inline (then nothing is ever pushed
        // and kicking them all IS the legacy face) or the first one was G458-culled (its bbox misses
        // the scissor: it draws nothing on any path). Keep kicking, in emission order, until one is
        // pushed; that one becomes the macro of the WHOLE face (the culled ones draw nothing natively
        // either: the native raster is scissored).
        prims.clear();
        if (g788FacePrims(*rec, face, prims, false))
        {
            size_t i = 1;
            for (; i < prims.size() && !h.consumed; ++i)
                g788KickPrim(gs, prims[i]);
            if (!h.consumed)
            {
                g_g788C.fallbackPrims.fetch_add(prims.size(), std::memory_order_relaxed);
                g_g788C.fallbackFaces.fetch_add(1u, std::memory_order_relaxed);
            }
            else
                g_g788C.lateMacros.fetch_add(i, std::memory_order_relaxed);
        }
    }
    h.mode = 0u;
    if (h.consumed)
        g_g788C.macros.fetch_add(1u, std::memory_order_relaxed);
    g788MarkLatchesDirty();
}

// ---- macro entry ---------------------------------------------------------------------------------
static inline bool g788IsMacro(const G144Entry &e)
{
    return e.prim.type == GS_PRIM_TRIANGLE && e.v[2].q == kG788MacroQ;
}
static inline uint32_t g788MacroSeq(const G144Entry &e)
{
    return static_cast<uint32_t>(e.v[2].u) | (static_cast<uint32_t>(e.v[2].v) << 16);
}

// Capture push site (parse thread): the face's first kicked triangle becomes the face's macro entry.
static GSVertex g788RefGs(const dc2_native::ReferenceVertex &r, uint16_t u, uint16_t v)
{
    GSVertex g{};
    g.x = static_cast<float>(r.x & 0xFFFFu) / 16.0f;
    g.y = static_cast<float>(r.y & 0xFFFFu) / 16.0f;
    g.z = static_cast<float>(r.z & 0xFFFFFFu);
    g.r = r.rgba[0];
    g.g = r.rgba[1];
    g.b = r.rgba[2];
    g.a = r.rgba[3];
    g.q = r.q == 0.0f ? 1.0f : r.q;
    g.s = r.s;
    g.t = r.t;
    g.u = u;
    g.v = v;
    g.fog = r.fog;
    return g;
}
static bool g788SameVertex(const GSVertex &a, const GSVertex &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a &&
           a.fog == b.fog && std::memcmp(&a.q, &b.q, 4) == 0 && std::memcmp(&a.s, &b.s, 4) == 0 &&
           std::memcmp(&a.t, &b.t, 4) == 0;
}
static void g788VerifyPush(const G144Entry &e)
{
    G788PushHook &h = g_g788Push;
    const uint32_t mode = h.clipMode & 3u;
    if (e.prim.type != GS_PRIM_TRIANGLE && e.prim.type != GS_PRIM_TRISTRIP && e.prim.type != GS_PRIM_TRIFAN)
    {
        g_g788C.vExtra.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    const double ax = e.v[0].x, ay = e.v[0].y;
    const double area = (double(e.v[1].x) - ax) * (double(e.v[2].y) - ay) - (double(e.v[1].y) - ay) * (double(e.v[2].x) - ax);
    if (g_g788VerifyTrace.size() < 1024u)
        g_g788VerifyTrace.push_back({int(e.prim.type), e.v[0].x, e.v[0].y, area == 0.0 ? 2u : 0u, {e.v[0], e.v[1], e.v[2]}});
    if (area == 0.0)
    {
        g_g788C.vZero.fetch_add(1u, std::memory_order_relaxed);
        g_g788C.vmZero[mode].fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    g_g788C.vmExpected[mode].fetch_add(1u, std::memory_order_relaxed);
    dc2_native::ReferencePrimitive scissorPrimitive;
    for (uint32_t k = 0; k < 3; ++k)
    {
        scissorPrimitive.vertices[k].x = uint32_t(e.v[k].x * 16.0f);
        scissorPrimitive.vertices[k].y = uint32_t(e.v[k].y * 16.0f);
    }
    if (g788Culled(scissorPrimitive, e.ctx, true))
        g_g788C.vmCulled[mode].fetch_add(1u, std::memory_order_relaxed);
    g_g788VerifyCtx = e.ctx;
    g_g788VerifyHaveCtx = true;
    while (h.cursor < g_g788VerifyPrims.size() && g788Culled(g_g788VerifyPrims[h.cursor], e.ctx))
        ++h.cursor;
    while (h.cursorB < g_g788VerifyPrimsB.size() && g788Culled(g_g788VerifyPrimsB[h.cursorB], e.ctx))
        ++h.cursorB;
    // Hypothesis B (clipping forced on): same rule, no logging.
    if (h.cursorB >= g_g788VerifyPrimsB.size())
    {
        h.faceBadB = true;
        g_g788C.vmMissingB[mode].fetch_add(1u, std::memory_order_relaxed);
    }
    else
    {
        const auto &pb = g_g788VerifyPrimsB[h.cursorB++];
        bool usedB[3] = {false, false, false};
        bool okB = true;
        for (int k = 0; k < 3; ++k)
        {
            const GSVertex r = g788RefGs(pb.vertices[static_cast<size_t>(k)], e.v[0].u, e.v[0].v);
            bool found = false;
            for (int m = 0; m < 3; ++m)
                if (!usedB[m] && g788SameVertex(r, e.v[m])) { usedB[m] = true; found = true; break; }
            if (!found) g_g788C.vmAttrB[mode].fetch_add(1u, std::memory_order_relaxed);
            okB &= found;
        }
        if (!okB)
            h.faceBadB = true;
    }
    if (h.cursor >= g_g788VerifyPrims.size())
    {
        g_g788C.vExtra.fetch_add(1u, std::memory_order_relaxed);
        g_g788C.vmMissingA[mode].fetch_add(1u, std::memory_order_relaxed);
        h.faceBad = true;
        return;
    }
    const auto &p = g_g788VerifyPrims[h.cursor++];
    bool used[3] = {false, false, false};
    bool ok = true;
    for (int k = 0; k < 3; ++k)
    {
        const GSVertex r = g788RefGs(p.vertices[static_cast<size_t>(k)], e.v[0].u, e.v[0].v);
        bool found = false;
        for (int m = 0; m < 3; ++m)
            if (!used[m] && g788SameVertex(r, e.v[m])) { used[m] = true; found = true; break; }
        if (!found) g_g788C.vmAttrA[mode].fetch_add(1u, std::memory_order_relaxed);
        ok &= found;
    }
    if (ok)
    {
        g_g788C.vOk.fetch_add(1u, std::memory_order_relaxed);
        if (!g_g788VerifyTrace.empty())
            g_g788VerifyTrace.back().matched = 1u;
    }
    else
    {
        h.faceBad = true;
        g_g788C.vBad.fetch_add(1u, std::memory_order_relaxed);
        if (g_g788VerifyLogs < 24u)
        {
            ++g_g788VerifyLogs;
            const auto &r0 = p.vertices[0];
            std::fprintf(stderr,
                         "[G788:vbad] seq=%u prim=%zu type=%d clipped=%u ref0=(%.4f,%.4f,%u rgba=%u,%u,%u,%u q=%.9g s=%.9g t=%.9g f=%u) "
                         "got0=(%.4f,%.4f,%.0f rgba=%u,%u,%u,%u q=%.9g s=%.9g t=%.9g f=%u)\n",
                         h.seq, h.cursor - 1u, int(e.prim.type), unsigned(p.clipped), (r0.x & 0xFFFFu) / 16.0, (r0.y & 0xFFFFu) / 16.0,
                         r0.z, r0.rgba[0], r0.rgba[1], r0.rgba[2], r0.rgba[3], double(r0.q), double(r0.s), double(r0.t), r0.fog,
                         double(e.v[0].x), double(e.v[0].y), double(e.v[0].z), e.v[0].r, e.v[0].g, e.v[0].b, e.v[0].a,
                         double(e.v[0].q), double(e.v[0].s), double(e.v[0].t), e.v[0].fog);
        }
    }
}
// drawPrimitive entry (rasterizer_tilebin_capture.inc top), verify mode only.
static void g788VerifyKick(const GSPrimReg &prim, const GSVertex &a, const GSVertex &b, const GSVertex &c,
                           const GSContext &ctx)
{
    G144Entry t;
    t.ctx = ctx;
    t.prim = prim;
    t.v[0] = a;
    t.v[1] = b;
    t.v[2] = c;
    g788VerifyPush(t);
}
static void g788OnPush(G144Entry &e)
{
    G788PushHook &h = g_g788Push;
    if (h.mode == 2u)
        return; // verify compares at drawPrimitive entry (g788VerifyKick)
    if ((h.mode != 1u && h.mode != 3u) || e.prim.type != GS_PRIM_TRIANGLE)
        return;
    e.v[2].q = kG788MacroQ;
    e.v[2].u = static_cast<uint16_t>(h.seq & 0xFFFFu);
    e.v[2].v = static_cast<uint16_t>(h.seq >> 16);
    // The face's screen extent is only known on the GPU: the conservative scissor rows.
    e.minY = static_cast<int>(e.ctx.scissor.y0);
    e.maxY = static_cast<int>(e.ctx.scissor.y1);
    h.consumed = 1u;
    h.mode = 0u;
}

// The exact legacy triangles of a macro (CPU reference): fn(entry, continuation) per primitive.
template <typename Fn>
static void g788ExpandEntry(const G144Entry &m, Fn &&fn)
{
    std::shared_ptr<const G788DrawRec> rec;
    uint32_t face = 0;
    if (!g788Lookup(g788MacroSeq(m), rec, face))
    {
        g_g788C.xStale.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    static thread_local std::vector<dc2_native::ReferencePrimitive> prims;
    prims.clear();
    if (!g788FacePrims(*rec, face, prims, false))
    {
        g_g788C.refPrimFail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    G144Entry t = m;
    const uint16_t u0 = m.v[0].u, v0 = m.v[0].v; // the (unwritten) UV register, as legacy latched it
    const int cofy = m.ctx.xyoffset.ofy >> 4;
    bool cont = false;
    for (const auto &p : prims)
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto &r = p.vertices[static_cast<size_t>(k)];
            GSVertex &v = t.v[k];
            v.x = static_cast<float>(r.x & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(r.y & 0xFFFFu) / 16.0f;
            v.z = static_cast<float>(r.z & 0xFFFFFFu);
            v.r = r.rgba[0];
            v.g = r.rgba[1];
            v.b = r.rgba[2];
            v.a = r.rgba[3];
            v.q = r.q == 0.0f ? 1.0f : r.q;
            v.s = r.s;
            v.t = r.t;
            v.u = u0;
            v.v = v0;
            v.fog = r.fog;
        }
        const float y0 = t.v[0].y - static_cast<float>(cofy), y1 = t.v[1].y - static_cast<float>(cofy),
                    y2 = t.v[2].y - static_cast<float>(cofy);
        const int lo = static_cast<int>(std::floor(std::min({y0, y1, y2})));
        const int hi = static_cast<int>(std::ceil(std::max({y0, y1, y2})));
        const int sy0 = static_cast<int>(t.ctx.scissor.y0), sy1 = static_cast<int>(t.ctx.scissor.y1);
        t.minY = lo < sy0 ? sy0 : (lo > sy1 ? sy1 : lo);
        t.maxY = hi < sy0 ? sy0 : (hi > sy1 ? sy1 : hi);
        fn(static_cast<const G144Entry &>(t), cont);
        cont = true;
    }
    g_g788C.xCpuFaces.fetch_add(1u, std::memory_order_relaxed);
}

// CPU replay (a refused GPU flush): the list goes back to exact legacy triangles.
static void g788ExpandListForCpu()
{
    bool any = false;
    for (const G144Entry &e : g_g144List)
        if (g788IsMacro(e)) { any = true; break; }
    if (!any)
        return;
    std::vector<G144Entry> out;
    out.reserve(g_g144List.size() + 256u);
    for (G144Entry &e : g_g144List)
    {
        if (!g788IsMacro(e)) { out.push_back(std::move(e)); continue; }
        g788ExpandEntry(e, [&](const G144Entry &t, bool) { out.push_back(t); });
    }
    g_g144List.swap(out);
    g_g788C.xCpuExpand.fetch_add(1u, std::memory_order_relaxed);
}

// After the vertex build: the batch's job table (output ranges in draw order) from its placeholders.
// Only adjacent native ranges, with all raster/sampler controls equal. GPU output is
// already in logical face order; joining contiguous ranges changes no primitive ordering.
static bool g788CompatibleRun(const G178Draw &a, const G178Draw &b)
{
    return a.g788Seq && b.g788Seq && a.g788Count && b.g788Count &&
        a.g788Out + a.g788Count == b.g788Out && a.firstVtx + a.vtxCount == b.firstVtx &&
        a.texKey == b.texKey && a.srcFbp == b.srcFbp && a.blend == b.blend &&
        a.tfx == b.tfx && a.tcc == b.tcc && a.fge == b.fge && a.fogCol == b.fogCol &&
        a.depthFunc == b.depthFunc && a.depthWrite == b.depthWrite && a.bilinear == b.bilinear &&
        a.wrapU == b.wrapU && a.wrapV == b.wrapV && a.uvOriginX == b.uvOriginX && a.uvOriginY == b.uvOriginY &&
        a.uvRoundU == b.uvRoundU && a.uvRoundV == b.uvRoundV &&
        a.uvPeriodU == b.uvPeriodU && a.uvPeriodV == b.uvPeriodV && a.screenSprite == b.screenSprite &&
        a.scX0 == b.scX0 && a.scY0 == b.scY0 && a.scX1 == b.scX1 && a.scY1 == b.scY1 &&
        a.g788Ofx == b.g788Ofx && a.g788Ofy == b.g788Ofy && a.g788Flags == b.g788Flags;
}
static void g788CoalesceRuns(G178Batch &b)
{
    // G791 release: DEFAULT ON (G788-AA setting); rollback DC2_G788_RUNS=0.
    static const bool enabled = [] { const char *e = std::getenv("DC2_G788_RUNS"); return !e || std::atoi(e); }();
    if (!enabled || b.g788Jobs.empty()) return;
    size_t written = 0;
    for (size_t i = 0; i < b.draws.size(); ++i)
    {
        const auto &d = b.draws[i];
        if (written && g788CompatibleRun(b.draws[written - 1u], d))
        {
            auto &previous = b.draws[written - 1u];
            previous.vtxCount += d.vtxCount;
            previous.g788Count += d.g788Count;
            g_g788C.xMerged.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            if (written != i) b.draws[written] = d;
            if (d.g788Seq) g_g788C.xRuns.fetch_add(1u, std::memory_order_relaxed);
            ++written;
        }
    }
    b.draws.resize(written);
}
static void g788BuildJobs(G178Batch &b)
{
    dc2_g788_econ::Scope econ(dc2_g788_econ::BuildJobs);
    b.g788Jobs.clear();
    uint32_t out = 0;
    for (G178Draw &d : b.draws)
    {
        if (d.g788Seq == 0u)
            continue;
        G788BatchJob j;
        if (!g788Lookup(d.g788Seq, j.rec, j.face) || !j.rec->mesh || j.face + 1u >= j.rec->mesh->faceTri.size())
        {
            d.g788Count = 0u; // draws nothing
            g_g788C.xStale.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        j.triCount = j.rec->mesh->faceTri[j.face + 1u] - j.rec->mesh->faceTri[j.face];
        j.outBase = out;
        j.flags = d.g788Flags;
        j.ofx = d.g788Ofx;
        j.ofy = d.g788Ofy;
        d.g788Out = out;
        d.g788Count = 24u * j.triCount;
        out += 24u * j.triCount;
        g_g788C.xTris.fetch_add(j.triCount, std::memory_order_relaxed);
        b.g788Jobs.push_back(std::move(j));
    }
    g_g788C.xJobs.fetch_add(b.g788Jobs.size(), std::memory_order_relaxed);
    g788CoalesceRuns(b);
}
