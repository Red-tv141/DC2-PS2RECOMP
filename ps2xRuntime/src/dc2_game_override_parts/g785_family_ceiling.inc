// G785: logical DRAW-FAMILY census and CEILING harness (plans/phase-G785-ledger.md).
//
// G784 measured that work volume (VU pairs, GIF bytes, parse) does not predict frame-time value, so
// a native migration target is selected by a ceiling probe first: drop the family's legacy output,
// draw nothing, A/B against the accepted baseline. This harness makes that probe one env flag.
//
// Two attribution levels (every wrapper chains to the previously registered function via
// lookupFunction, so the G56/G58/G59/G67 taps and the G775 capture on the same addresses still run):
//
// 1. GAME-LEVEL families (CMap, CMapPiece, CObjectFrame, CCharacter2, effect managers, ...). MEASURED
//    (g785a_cls census): these calls only QUEUE their frames -- every MDT draw and AddPacket node runs
//    later, outside their call extent -- so a dynamic-extent family mask attributes nothing. Their
//    lever is DC2_G785_DROP_MODE=call: the whole game-level Draw is skipped (nothing queued => EE +
//    VU + GS of the family deleted; a ceiling that also includes the family's EE draw preparation).
// 2. SEAM KEYS at the visual seams (mgCVisualMDT::Draw dest 0x13f4e0 and the water / mgCSprite /
//    mgC3DSprite dest Draws): key = (visual vtable, OWNER family mask). The owner is recorded when a
//    game-level Draw queues a frame (mgDraw / mgDrawDirect / mgDrawDirect2, inside its call extent)
//    and found at the seam by walking the dispatching frame (s5 at mgCFrame::Draw's 0x1380e4) up its
//    parent links (+0x54) to the nearest queued ancestor. (Visual+parent vtables alone were measured
//    useless: on MAP15 one key held 98 % of draws.) Plus the visual `self`. A dropped key returns 0 qwords written: no state packet, no AddPacket node, no VIF /
//    VU1 / GIF / GS work; the EE frame traversal above it still runs.
// Env (none of them is a correctness configuration):
//   DC2_G785_FAMILY=1          census: cumulative [G785:family] and [G785:vkey] lines every 600 SF
//   DC2_G785_DROP=a,b,...|none CEILING probe. Tokens: a game family name (with DROP_MODE=call), a
//                              seam name (mdt, water, sprite, sprite3d: every draw of that seam),
//                              vvt:<hex> (visual vtable), pvt:<hex> (parent frame vtable),
//                              self:<hex> (one visual). `none` registers the wrappers and drops
//                              nothing -- the symmetric control arm.
//   DC2_G785_DROP_MODE=call    game-family tokens skip the whole game-level Draw call
namespace g785 {
using RecompiledFunction = PS2Runtime::RecompiledFunction;
struct Family { uint32_t addr; const char* name; };
// Game-level families. Index = mask bit; index 0 is reserved for "no family". DC2_G785_FAMILY_ADD=hex:name,...
// appends more at run time (up to kMaxFamilies) without a rebuild.
constexpr Family kBaseFamilies[] = {
    {0u,          "none"},
    {0x00160B10u, "map"},          // Draw__4CMapFv
    {0x0015E3D0u, "mapparts"},     // Draw__9CMapPartsFv
    {0x00166E40u, "mappiece"},     // Draw__9CMapPieceFv
    {0x00160B50u, "object"},       // Draw__7CObjectFv
    {0x00169FD0u, "objframe"},     // Draw__12CObjectFrameFv
    {0x00138840u, "mgobject"},     // Draw__9mgCObjectFv
    {0x00172F60u, "chara2"},       // Draw__11CCharacter2Fv
    {0x0016B850u, "actionchara"},  // Draw__12CActionCharaFv
    {0x0017C2C0u, "outline"},      // Draw__12COutLineDrawFPfff
    {0x0017C2D0u, "outline2"},     // Draw__12COutLineDrawFff
    {0x00182DC0u, "effectman"},    // Draw__14CEffectManagerFv
    {0x0017FF70u, "effect"},       // Draw__7CEffectFv
    {0x001C5AE0u, "battleeff"},    // Draw__15BattleEffectManFv
    {0x001C4C70u, "mapeffects"},   // Draw__18CMapEffectsManegerFP9mgCCamera
    {0x001C4440u, "mapeffsprite"}, // Draw__17CMapEffect_SpriteFP9mgCCameraP10CPreSprite
    {0x002E19B0u, "effscript"},    // Draw__16CEffectScriptManFv
    {0x002829E0u, "rain"},         // Draw__5CRainFv
    {0x00281B10u, "particle"},     // Draw__9CParticleFv
    {0x002815E0u, "ripple"},       // Draw__7CRippleFv
    {0x00281F60u, "raindrop"},     // Draw__9CRainDropFv
    {0x00260640u, "screeneff"},    // Draw__13CScreenEffectFv
    {0x0017DED0u, "fade"},         // Draw__10CFadeInOutFv
    {0x0028C7B0u, "treasure"},     // Draw__19CTreasureBoxManagerFPf
    {0x001C2F20u, "flusheff"},     // Draw__12CFlushEffectFv
    {0x001C1070u, "minieffman"},   // Draw__15CMiniEffPrimManFv
    {0x001C0E20u, "minieffprim"},  // Draw__12CMiniEffPrimFP10CPreSprite
    {0x00290900u, "eventsprite"},  // Draw__18CEventSpriteMotherFv
    {0x002E9180u, "powgage"},      // Draw__8CPowGageFv
    {0x0020CFD0u, "bubble"},       // Draw__7CBubbleFv
    {0x00217B10u, "aquarium"},     // Draw__9CAquariumFv
    {0x002EB790u, "sphida"},       // Draw__7CSphidaFv
    {0x002FC360u, "placeanime"},   // Draw__11CPlaceAnimeFv
    {0x002F0B40u, "editevent"},    // Draw__10CEditEventFP6CScene
    {0x001C1A60u, "swordlum"},     // Draw__14CSwordLuminousFv
    {0x001C1D30u, "swordafter"},   // Draw__16CSWordAfterImageFv
    {0x001C3AE0u, "deadeff"},      // Draw__11CDeadEffectFv
    // Immediate (direct-DMA) draws: they never reach the visual seams, so only DROP_MODE=call removes them.
    {0x001731F0u, "chara2direct"}, // DrawDirect__11CCharacter2Fv
    {0x00173610u, "chara2shadow"}, // DrawShadowDirect__11CCharacter2Fv
    {0x00177CB0u, "chara2effect"}, // DrawEffect__11CCharacter2Fv
    {0x0017ACB0u, "dynanime"},     // DrawSub__13CDynamicAnimeFi
    {0x0016A010u, "objframedirect"},// DrawDirect__12CObjectFrameFv
    {0x001832F0u, "sky"},          // DrawSky__7CMapSkyFPfPfPfiPfPf
    {0x001831F0u, "skyback"},      // DrawSkyBack__7CMapSkyFPfPfPf
    {0x0015E800u, "mapwater"},     // DrawWater__4CMapFP9mgCCameraP10mgCTextureP10mgCTexture
    {0x0015E3F0u, "mapeffect"},    // DrawEffect__4CMapFv
    {0x002FB3A0u, "stareff"},      // Draw__11CStarEffectFv
    {0x002FB850u, "painteff"},     // Draw__12CPaintEffectFv
    {0x001C0AB0u, "sparceff"},     // Draw__12CSparcEffectFv
    {0x001C05E0u, "thunder"},      // Draw__8CThunderFv
    {0x001C00E0u, "tornado"},      // Draw__8CTornadoFv
    {0x001B77A0u, "lasergun"},     // Draw__9CLaserGunFv
    {0x001B6560u, "rocket"},       // Draw__15CRocketLauncherFv
    {0x002F7D80u, "firepowder"},   // DrawFirePowder__FP6CScene
    {0x002F8B20u, "geyser"},       // DrawGeyserEffect__FP6CScene
    {0x0029E3C0u, "fireeffect"},   // DrawFireEffect__FPA4_fP14CFuncPointMngr...
    {0x002FE150u, "fishing"},      // sgDrawFishing__FP11SubGameInfo
};
constexpr uint32_t kBaseFamilyCount = sizeof(kBaseFamilies) / sizeof(kBaseFamilies[0]);
constexpr uint32_t kMaxFamilies = 60; // bits 0..59; 60..63 are packet seams
static_assert(kBaseFamilyCount <= kMaxFamilies, "family bits");
inline Family kFamilies[kMaxFamilies]{};
inline std::string g_familyNameStore[kMaxFamilies];
inline uint32_t kFamilyCount = 0;
inline void initFamilies()
{
    if (kFamilyCount) return;
    for (uint32_t i = 0; i < kBaseFamilyCount; ++i) kFamilies[i] = kBaseFamilies[i];
    kFamilyCount = kBaseFamilyCount;
    const char *add = std::getenv("DC2_G785_FAMILY_ADD");
    std::string spec = add ? add : "";
    size_t pos = 0;
    while (pos < spec.size() && kFamilyCount < kMaxFamilies)
    {
        size_t end = spec.find_first_of(",+", pos); // '+' too: the route launcher truncates at ','
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            g_familyNameStore[kFamilyCount] = item.substr(colon + 1);
            kFamilies[kFamilyCount] = {uint32_t(std::strtoul(item.c_str(), nullptr, 16)) & 0x1fffffffu,
                                       g_familyNameStore[kFamilyCount].c_str()};
            ++kFamilyCount;
        }
        pos = end + 1;
    }
}
// Packet seams (bit 60 + index).
enum Seam : uint32_t { SeamMdt = 0, SeamWater, SeamSprite, SeamSprite3d, SeamCount };
constexpr Family kSeams[SeamCount] = {
    {0x0013F4E0u, "mdt"},      // Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager (geometry = AddPacket)
    {0x001851A0u, "water"},    // Draw__6CWaterFPUiPA4_fP14mgCDrawManager
    {0x0013BB70u, "sprite"},   // Draw__9mgCSpriteFPUiPA4_fP14mgCDrawManager
    {0x0013B070u, "sprite3d"}, // Draw__11mgC3DSpriteFPUiPA4_fP14mgCDrawManager
};
constexpr uint32_t kAddPacket = 0x001359D0u;

inline bool censusOn() { static const bool on = std::getenv("DC2_G785_FAMILY") != nullptr; return on; }
inline const char *dropSpec() { static const char *v = std::getenv("DC2_G785_DROP"); return v; }
inline bool callMode()
{
    static const bool on = [] { const char *v = std::getenv("DC2_G785_DROP_MODE"); return v && !std::strcmp(v, "call"); }();
    return on;
}
// G786: COutLineDraw::Draw(ff) census (calls, work texture, parameters) on the EE side.
inline bool g786EeOn() { static const bool on = std::getenv("DC2_G786_OLCENSUS") != nullptr; return on; }
// G786 ceiling probe (TIMING ONLY, visibly removes outlines): DC2_G786_DROPTBP=<hex>[+<hex>...] skips
// COutLineDraw::Draw(ff) for objects whose work texture tbp (tex+0x38 & 0x3fff) is listed.
inline const std::vector<uint32_t> &g786DropTbps()
{
    static const std::vector<uint32_t> v = [] {
        std::vector<uint32_t> out;
        const char *e = std::getenv("DC2_G786_DROPTBP");
        std::string spec = e ? e : "";
        size_t pos = 0;
        while (pos < spec.size())
        {
            size_t end = spec.find_first_of(",+ ", pos);
            if (end == std::string::npos) end = spec.size();
            if (end > pos) out.push_back(uint32_t(std::strtoul(spec.substr(pos, end - pos).c_str(), nullptr, 16)));
            pos = end + 1;
        }
        return out;
    }();
    return v;
}
// G786 ceiling probes (TIMING ONLY, visibly corrupt outlines): DC2_G786_OLSKIP=<hex mask>, effective only
// inside COutLineDraw::Draw(ff): bit0 the silhouette mgDraw* (render-only), bit1 DrawDivSprite4 (outline
// composite), bit2 DrawDivSprite (centre composite).
inline uint32_t g786OlSkip()
{
    static const uint32_t v = [] {
        const char *e = std::getenv("DC2_G786_OLSKIP");
        return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    return v;
}
inline bool armed() { return censusOn() || dropSpec() != nullptr || g786EeOn() || !g786DropTbps().empty() || g786OlSkip() != 0u; }
struct DropSpec { uint64_t mask = 0; std::vector<uint32_t> vvt, pvt, self; };
inline const DropSpec &drops()
{
    static const DropSpec spec = [] {
        DropSpec d;
        const char *v = dropSpec();
        if (!v || !std::strcmp(v, "none")) return d;
        std::string text(v);
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t end = text.find_first_of(",+ ", pos);
            if (end == std::string::npos) end = text.size();
            const std::string name = text.substr(pos, end - pos);
            bool found = name.empty();
            const auto hexAfter = [&name](size_t n) { return uint32_t(std::strtoul(name.c_str() + n, nullptr, 16)) & 0x1fffffffu; };
            if (!name.compare(0, 4, "vvt:")) { d.vvt.push_back(hexAfter(4)); found = true; }
            else if (!name.compare(0, 4, "pvt:")) { d.pvt.push_back(hexAfter(4)); found = true; }
            else if (!name.compare(0, 5, "self:")) { d.self.push_back(hexAfter(5)); found = true; }
            for (uint32_t i = 1; i < kFamilyCount; ++i)
                if (name == kFamilies[i].name) { d.mask |= 1ull << i; found = true; }
            for (uint32_t s = 0; s < SeamCount; ++s)
                if (name == kSeams[s].name) { d.mask |= 1ull << (60u + s); found = true; }
            if (!found) { std::fprintf(stderr, "[G785:drop] FATAL unknown family '%s'\n", name.c_str()); std::abort(); }
            pos = end + 1;
        }
        std::fprintf(stderr, "[G785:drop] spec=%s mask=%016llx vvt=%zu pvt=%zu self=%zu mode=%s\n", v,
                     (unsigned long long)d.mask, d.vvt.size(), d.pvt.size(), d.self.size(),
                     callMode() ? "call" : "packet");
        return d;
    }();
    return spec;
}
inline uint64_t dropMask() { return drops().mask; }
inline bool listed(const std::vector<uint32_t> &list, uint32_t value)
{
    for (uint32_t x : list) if (x == value) return true;
    return false;
}

// ---- seam keys: (seam, visual vtable, OWNER family mask), open addressing, EE thread only -------
// The owner mask is the family mask that was active when the visual's frame (or its nearest registered
// ancestor, parent link mgCFrame+0x54) was queued by mgDraw / mgDrawDirect / mgDrawDirect2 inside a
// game-level Draw, OR'ed with any family whose call extent is live at the seam (immediate draws).
struct KeyStat { uint32_t vvt = 0, used = 0, lastSelf = 0, seam = 0; uint64_t owner = 0, draws = 0, qw = 0, nodes = 0, drops = 0, selves = 0; };
constexpr uint32_t kKeySlots = 1024;
inline KeyStat keyStats[kKeySlots];
inline int g_curKey = -1; // the seam key whose dest Draw is executing (AddPacket attribution)
inline int keySlot(uint32_t seam, uint32_t vvt, uint64_t owner)
{
    uint32_t h = (vvt * 0x9E3779B1u) ^ (uint32_t(owner ^ (owner >> 32)) * 0x85EBCA77u) ^ seam;
    for (uint32_t probe = 0; probe < kKeySlots; ++probe)
    {
        KeyStat &k = keyStats[(h + probe) & (kKeySlots - 1u)];
        if (!k.used) { k.used = 1; k.vvt = vvt; k.owner = owner; k.seam = seam; return int((h + probe) & (kKeySlots - 1u)); }
        if (k.vvt == vvt && k.owner == owner && k.seam == seam) return int((h + probe) & (kKeySlots - 1u));
    }
    return -1;
}
// frame -> owner family mask, recorded at queue time (EE thread only). Open addressing, overwrite.
constexpr uint32_t kFrameSlots = 8192;
struct FrameOwner { uint32_t frame = 0; uint64_t mask = 0; };
inline FrameOwner frameOwners[kFrameSlots];
inline void noteFrameOwner(uint32_t frame, uint64_t mask)
{
    if (!frame || !mask) return;
    uint32_t h = (frame >> 4) * 0x9E3779B1u;
    for (uint32_t probe = 0; probe < 64u; ++probe)
    {
        FrameOwner &f = frameOwners[(h + probe) & (kFrameSlots - 1u)];
        if (!f.frame || f.frame == frame) { f.frame = frame; f.mask = mask; return; }
    }
}
inline uint64_t frameOwner(uint32_t frame)
{
    uint32_t h = (frame >> 4) * 0x9E3779B1u;
    for (uint32_t probe = 0; probe < 64u; ++probe)
    {
        const FrameOwner &f = frameOwners[(h + probe) & (kFrameSlots - 1u)];
        if (f.frame == frame) return f.mask;
        if (!f.frame) return 0;
    }
    return 0;
}
inline uint64_t ownerOf(uint8_t *rdram, uint32_t frame)
{
    for (uint32_t depth = 0; frame && frame < 0x02000000u && depth < 32u; ++depth)
    {
        if (const uint64_t m = frameOwner(frame)) return m;
        frame = dc2_read_u32(rdram, frame + 0x54u) & 0x1fffffffu;
    }
    return 0;
}
inline std::string familyNames(uint64_t mask)
{
    std::string s;
    for (uint32_t i = 1; i < kFamilyCount; ++i)
        if (mask & (1ull << i)) { if (!s.empty()) s += '+'; s += kFamilies[i].name; }
    return s.empty() ? std::string("none") : s;
}

// EE thread only (every wrapped function and AddPacket run on the EE).
struct Counters { uint64_t calls = 0, nodes = 0, droppedNodes = 0, seam[SeamCount]{}, seamQw[SeamCount]{}, droppedSeams = 0, droppedCalls = 0; };
inline Counters counters[kMaxFamilies];
inline uint64_t g_mask = 0;     // OR of every family on the current EE call stack
inline uint32_t g_innermost = 0; // census attribution: innermost game family
inline RecompiledFunction g_prevFamily[kMaxFamilies]{};
inline RecompiledFunction g_prevSeam[SeamCount]{};
inline RecompiledFunction g_prevAddPacket = nullptr;

constexpr uint32_t kQueueFns[3] = {0x00142F90u, 0x00142FD0u, 0x001430E0u};
inline RecompiledFunction g_prevQueue[3]{};
inline uint64_t g_queueCalls[3]{};
inline uint64_t g_queueDrops = 0;
inline void maybeReport()
{
    if (!censusOn()) return;
    static uint32_t last = 0;
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf == last || sf % 600u != 0u) return;
    last = sf;
    for (uint32_t i = 0; i < kFamilyCount; ++i)
    {
        const Counters &c = counters[i];
        if (!c.calls && !c.nodes && !c.seam[0] && !c.seam[1] && !c.seam[2] && !c.seam[3]) continue;
        std::fprintf(stderr,
                     "[G785:family] sf=%u fam=%s calls=%llu nodes=%llu mdt=%llu water=%llu sprite=%llu sprite3d=%llu "
                     "mdtQw=%llu waterQw=%llu spriteQw=%llu sprite3dQw=%llu dropNodes=%llu dropSeams=%llu dropCalls=%llu\n",
                     sf, kFamilies[i].name, (unsigned long long)c.calls, (unsigned long long)c.nodes,
                     (unsigned long long)c.seam[0], (unsigned long long)c.seam[1], (unsigned long long)c.seam[2],
                     (unsigned long long)c.seam[3], (unsigned long long)c.seamQw[0], (unsigned long long)c.seamQw[1],
                     (unsigned long long)c.seamQw[2], (unsigned long long)c.seamQw[3],
                     (unsigned long long)c.droppedNodes, (unsigned long long)c.droppedSeams,
                     (unsigned long long)c.droppedCalls);
    }
    std::fprintf(stderr, "[G785:queue] sf=%u mgDraw=%llu mgDrawDirect=%llu mgDrawDirect2=%llu drops=%llu\n", sf,
                 (unsigned long long)g_queueCalls[0], (unsigned long long)g_queueCalls[1],
                 (unsigned long long)g_queueCalls[2], (unsigned long long)g_queueDrops);
    for (uint32_t i = 0; i < kKeySlots; ++i)
    {
        const KeyStat &k = keyStats[i];
        if (!k.used) continue;
        std::fprintf(stderr,
                     "[G785:vkey] sf=%u seam=%s vvt=%x owner=%s draws=%llu qw=%llu nodes=%llu drops=%llu selves=%llu lastSelf=%x\n",
                     sf, kSeams[k.seam].name, k.vvt, familyNames(k.owner).c_str(), (unsigned long long)k.draws, (unsigned long long)k.qw,
                     (unsigned long long)k.nodes, (unsigned long long)k.drops, (unsigned long long)k.selves, k.lastSelf);
    }
}

struct G786Outline
{
    uint64_t calls = 0, noFrame = 0, noTex = 0;
    uint32_t tbp[4]{}; uint64_t tbpN[4]{};
    uint32_t self[16]{}; uint64_t selfN[16]{}; uint32_t nSelf = 0; uint64_t selfOverflow = 0;
    float f12lo = 1e30f, f12hi = -1e30f, f13lo = 1e30f, f13hi = -1e30f, p38lo = 1e30f, p38hi = -1e30f;
    uint32_t lastSf = 0;
};
inline G786Outline g786Ol;
inline void g786NoteOutline(uint8_t *rdram, R5900Context *ctx)
{
    G786Outline &o = g786Ol;
    ++o.calls;
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
    const uint32_t frame = dc2_read_u32(rdram, self + 0x34u) & 0x1fffffffu;
    const uint32_t tex = dc2_read_u32(rdram, self + 0x30u) & 0x1fffffffu;
    if (!frame) ++o.noFrame;
    if (!tex) ++o.noTex;
    const uint32_t tbp = tex ? (dc2_read_u32(rdram, tex + 0x38u) & 0x3fffu) : 0u;
    for (uint32_t k = 0; k < 4u; ++k)
        if (o.tbp[k] == tbp || o.tbpN[k] == 0u) { o.tbp[k] = tbp; ++o.tbpN[k]; break; }
    uint32_t k = 0;
    for (; k < o.nSelf; ++k) if (o.self[k] == self) break;
    if (k < o.nSelf) ++o.selfN[k];
    else if (o.nSelf < 16u) { o.self[o.nSelf] = self; o.selfN[o.nSelf++] = 1u; }
    else ++o.selfOverflow;
    float p38;
    const uint32_t w38 = dc2_read_u32(rdram, self + 0x38u);
    std::memcpy(&p38, &w38, 4);
    o.f12lo = std::min(o.f12lo, ctx->f[12]); o.f12hi = std::max(o.f12hi, ctx->f[12]);
    o.f13lo = std::min(o.f13lo, ctx->f[13]); o.f13hi = std::max(o.f13hi, ctx->f[13]);
    o.p38lo = std::min(o.p38lo, p38); o.p38hi = std::max(o.p38hi, p38);
    const uint32_t sf = g_dc2ScriptFrame.load(std::memory_order_relaxed);
    if (sf / 600u != o.lastSf / 600u)
    {
        o.lastSf = sf;
        std::fprintf(stderr,
                     "[G786:ee] sf=%u calls=%llu noFrame=%llu noTex=%llu tbp=%x:%llu,%x:%llu,%x:%llu,%x:%llu "
                     "selves=%u overflow=%llu f12=%.4f..%.4f f13=%.4f..%.4f p38=%.4f..%.4f\n",
                     sf, (unsigned long long)o.calls, (unsigned long long)o.noFrame, (unsigned long long)o.noTex,
                     o.tbp[0], (unsigned long long)o.tbpN[0], o.tbp[1], (unsigned long long)o.tbpN[1],
                     o.tbp[2], (unsigned long long)o.tbpN[2], o.tbp[3], (unsigned long long)o.tbpN[3],
                     o.nSelf, (unsigned long long)o.selfOverflow, o.f12lo, o.f12hi, o.f13lo, o.f13hi,
                     o.p38lo, o.p38hi);
    }
}

template <uint32_t I>
void familyWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if constexpr (I == 10u) // outline2 = Draw__12COutLineDrawFff (kBaseFamilies index 10)
    {
        if (g786EeOn()) g786NoteOutline(rdram, ctx);
        if (!g786DropTbps().empty())
        {
            const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
            const uint32_t tex = dc2_read_u32(rdram, self + 0x30u) & 0x1fffffffu;
            const uint32_t tbp = tex ? (dc2_read_u32(rdram, tex + 0x38u) & 0x3fffu) : 0u;
            for (uint32_t x : g786DropTbps())
                if (x == tbp)
                {
                    ++counters[I].droppedCalls;
                    setReturnU32(ctx, 0u);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
        }
    }
    const uint64_t bit = 1ull << I;
    Counters &c = counters[I];
    ++c.calls;
    maybeReport();
    if (callMode() && (dropMask() & bit))
    {
        ++c.droppedCalls;
        ctx->pc = getRegU32(ctx, 31); // the whole game-level Draw is skipped: nothing is queued
        return;
    }
    const uint64_t savedMask = g_mask;
    const uint32_t savedInner = g_innermost;
    g_mask |= bit;
    g_innermost = I;
    g_prevFamily[I](rdram, ctx, runtime);
    g_mask = savedMask;
    g_innermost = savedInner;
}

// Every visual seam is a dest-form Draw(self=a0, dest=a1, matrix=a2, manager=a3) that returns the
// qwords it wrote. mgCFrame::Draw(uint*) dispatches it from 0x1380dc with the frame in s5.
template <uint32_t S>
void seamWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    maybeReport();
    Counters &c = counters[g_innermost];
    ++c.seam[S];
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
    const uint32_t vvt = self ? (dc2_read_u32(rdram, self + 0x1cu) & 0x1fffffffu) : 0u;
    const uint32_t frame = ra == 0x001380e4u ? (getRegU32(ctx, 21) & 0x1fffffffu) : 0u;
    const uint64_t owner = g_mask | (frame ? ownerOf(rdram, frame) : 0ull);
    const uint32_t pvt = frame ? (dc2_read_u32(rdram, frame) & 0x1fffffffu) : (0x80000000u | ra);
    const int slot = keySlot(S, vvt, owner);
    KeyStat *k = slot >= 0 ? &keyStats[slot] : nullptr;
    if (k)
    {
        ++k->draws;
        if (k->lastSelf != self) { ++k->selves; k->lastSelf = self; }
    }
    const DropSpec &d = drops();
    // Game-family tokens drop at the seam only in packet mode (call mode skips their queueing call
    // instead); seam / vvt / pvt / self tokens drop here in either mode, so one spec can mix both.
    const bool drop = (!callMode() && (d.mask & owner)) || (d.mask & (1ull << (60u + S))) ||
                      listed(d.vvt, vvt) || listed(d.pvt, pvt) || listed(d.self, self);
    if (drop)
    {
        ++c.droppedSeams;
        if (k) ++k->drops;
        setReturnU32(ctx, 0u); // zero qwords written into the destination chain, no AddPacket
        ctx->pc = ra;
        return;
    }
    const int savedKey = g_curKey;
    g_curKey = slot;
    g_prevSeam[S](rdram, ctx, runtime);
    g_curKey = savedKey;
    if (ctx->pc == ra)
    {
        c.seamQw[S] += getRegU32(ctx, 2);
        if (k) k->qw += getRegU32(ctx, 2);
    }
}

// Frame queue functions called directly from game-level Draws (inside their call extent):
// mgDraw(mgCFrame*), mgDrawDirect(mgCFrame*), mgDrawDirect2(mgCFrame*). a0 = the frame.
template <uint32_t Q>
void queueWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++g_queueCalls[Q];
    // RENDER-ONLY drop (packet mode): a dropped family's frame is neither queued nor drawn
    // immediately, but the family's game-level Draw still runs its EE preparation (animation,
    // LOD, deformation setup). mgDraw* return v0 = qwords (immediate) or 0: return 0.
    if ((!callMode() && (dropMask() & g_mask & ((1ull << 60) - 1ull))) ||
        ((g786OlSkip() & 1u) && (g_mask & (1ull << 10)))) // G786: outline2 = family bit 10
    {
        ++g_queueDrops;
        setReturnU32(ctx, 0u);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    noteFrameOwner(getRegU32(ctx, 4) & 0x1fffffffu, g_mask);
    g_prevQueue[Q](rdram, ctx, runtime);
}

// G786: DrawDivSprite4 (0x17CB20) / DrawDivSprite (0x17C8F0), skipped only inside COutLineDraw::Draw(ff).
inline RecompiledFunction g786PrevDiv[2]{};
inline uint64_t g786DivSkips[2]{};
template <uint32_t D>
void g786DivWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if ((g786OlSkip() & (2u << D)) && (g_mask & (1ull << 10)))
    {
        ++g786DivSkips[D];
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g786PrevDiv[D](rdram, ctx, runtime);
}

inline void addPacketWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    Counters &c = counters[g_innermost];
    ++c.nodes;
    if (g_curKey >= 0) ++keyStats[g_curKey].nodes;
    g_prevAddPacket(rdram, ctx, runtime);
}

template <uint32_t... I>
void registerFamilies(PS2Runtime &runtime, std::integer_sequence<uint32_t, I...>)
{
    const RecompiledFunction wrappers[] = {&familyWrapper<I>...};
    for (uint32_t i = 1; i < kFamilyCount; ++i)
    {
        if (!kFamilies[i].addr) continue;
        g_prevFamily[i] = runtime.lookupFunction(kFamilies[i].addr);
        if (!g_prevFamily[i]) { std::fprintf(stderr, "[G785:family] FATAL no function at %x\n", kFamilies[i].addr); std::abort(); }
        runtime.registerFunction(kFamilies[i].addr, wrappers[i]);
    }
}
} // namespace g785

static void g785_register_families(PS2Runtime &runtime)
{
    if (!g785::armed()) return;
    g785::initFamilies();   // base table + DC2_G785_FAMILY_ADD, before the drop spec names them
    (void)g785::dropMask(); // parse (and validate) the spec at startup
    g785::registerFamilies(runtime, std::make_integer_sequence<uint32_t, g785::kMaxFamilies>{});
    const g785::RecompiledFunction seams[g785::SeamCount] = {&g785::seamWrapper<0>, &g785::seamWrapper<1>,
                                                       &g785::seamWrapper<2>, &g785::seamWrapper<3>};
    for (uint32_t s = 0; s < g785::SeamCount; ++s)
    {
        g785::g_prevSeam[s] = runtime.lookupFunction(g785::kSeams[s].addr);
        if (!g785::g_prevSeam[s]) { std::fprintf(stderr, "[G785:family] FATAL no seam at %x\n", g785::kSeams[s].addr); std::abort(); }
        runtime.registerFunction(g785::kSeams[s].addr, seams[s]);
    }
    const g785::RecompiledFunction queues[3] = {&g785::queueWrapper<0>, &g785::queueWrapper<1>, &g785::queueWrapper<2>};
    for (uint32_t q = 0; q < 3u; ++q)
    {
        g785::g_prevQueue[q] = runtime.lookupFunction(g785::kQueueFns[q]);
        runtime.registerFunction(g785::kQueueFns[q], queues[q]);
    }
    g785::g_prevAddPacket = runtime.lookupFunction(g785::kAddPacket);
    if (!g785::g_prevAddPacket) { std::fprintf(stderr, "[G785:family] FATAL no AddPacket\n"); std::abort(); }
    runtime.registerFunction(g785::kAddPacket, &g785::addPacketWrapper);
    if (g785::g786OlSkip() & 6u)
    {
        const uint32_t divAddr[2] = {0x0017CB20u, 0x0017C8F0u}; // DrawDivSprite4, DrawDivSprite
        const g785::RecompiledFunction divs[2] = {&g785::g786DivWrapper<0>, &g785::g786DivWrapper<1>};
        for (uint32_t d = 0; d < 2u; ++d)
        {
            g785::g786PrevDiv[d] = runtime.lookupFunction(divAddr[d]);
            if (!g785::g786PrevDiv[d]) { std::fprintf(stderr, "[G786:skip] FATAL no function at %x\n", divAddr[d]); std::abort(); }
            runtime.registerFunction(divAddr[d], divs[d]);
        }
    }
    if (g785::g786OlSkip() || !g785::g786DropTbps().empty())
        std::fprintf(stderr, "[G786:probe] olskip=0x%x droptbp=%zu\n", g785::g786OlSkip(), g785::g786DropTbps().size());
    std::fprintf(stderr, "[G785:family] armed families=%u seams=%u census=%d\n", g785::kFamilyCount - 1u,
                 (unsigned)g785::SeamCount, g785::censusOn() ? 1 : 0);
}
