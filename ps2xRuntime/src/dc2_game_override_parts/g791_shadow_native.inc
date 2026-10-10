// G791 — NATIVE ShadowMDT (plans/phase-G791-ledger.md).
//
// mgCShadowMDT (vtable 0x375020) draws a character's ground shadow as a Z-pass stencil VOLUME kept in a
// colour channel (G761): every source triangle of the rigid shadow mesh is extruded by the mg shadow
// VU1 microprogram (ELF 0x003222D0, micro 100; disassembly logs/g791/vu_shadow_dis.txt) into a prism
// of five 4-vertex TRIFANs; each fan is XGKICK'd under ALPHA 0x68 (ADD) or 0x62 (SUB) by its screen
// winding, in an order that matters under COLCLAMP.
//
// Legacy, per draw (Draw__12mgCVisualMDT dest form, 0x13F4E0):
//   CALL renderInfo   UNPACK VU0..32 (matrices) -> MSCAL 0 (the shared prologue ONLY: its JR's delay
//                     slot carries the E bit) -> DIRECT GS env -> SetShadowData UNPACK VU40..47
//                     (shadow matrix + the two A+D ALPHA headers) -> RET
//   per bucket        mgSendVuProg + CALL chain; chain = one REF per face (the face packet
//                     CreateFacePacket rebuilt this frame: per <=42-triangle batch UNPACK FLG
//                     [tag, (n,n,hdr), 3n vertices] + MSCAL 2) + CNT TEXFLUSH + RET.
//
// Native (DC2_G791_SHADOW=native; DEFAULT OFF, see kDefaultMode): at the dest loop's mgSendVuProg entry (return site 0x13F5A0,
// Draw's s0 = the bucket) every supported face's REF is replaced by a REF to a 2-qword marker
// (lib/ps2_g791_shadow_api.inc) whose host ring slot holds the EXACT GIF bytes the microprogram would have
// XGKICK'd, computed on the EE from
//   live rigid source     self+0x30 vertex array through the face's triangle-list indices (type 3), read at
//                         the seam exactly as CreateFacePacket did moments earlier (not frozen: see source());
//   exact current state   VU25..32 / VU8..14 / VU40..47 read out of THIS draw's renderInfo packet (built by
//                         CreateRenderInfoPacket + SetShadowData moments earlier), the prologue's light basis
//                         recomputed with the VU interpreter's float semantics (separate rounded mul/add, IEEE
//                         div/sqrt, truncating FTOI4).
// The face's VIF unpack, its VU1 kick(s) and PATH1 vanish; the parse thread meets the marker at the face's
// stream position (the arbiter seals pending kicks ahead of a PATH2 window) and parses the slot's bytes right
// there, so the 0x139 winding target is rasterised from an identical stream. renderInfo, CreatePacket, mgSendVuProg, the chain's TEXFLUSH,
// mgBeginDrawShadow / mgEndDrawShadow stay legacy. A face whose inputs are not the proven contract (non-type-3,
// non-finite values, w == 0, FTOI4 saturation, an unexpected packet shape, no packet memory) keeps its legacy
// REF: every face has exactly one owner.
//
// Verify (DC2_G791_SHADOW=verify, diagnostic build only): the legacy face REF is kept and bracketed by two
// DIRECT 1 marker tags (copy of the face packet in between); the GS front door captures the PATH1 bytes the
// VU really kicked between the markers and compares them with the native bytes (lib/ps2_g791_shadow_oracle.inc).
// The face packet input (tag template, header, every vertex) is compared with the native source on the EE.
namespace g791 {
using RecompiledFunction = PS2Runtime::RecompiledFunction;

constexpr uint32_t kShadowVtable = 0x375020u;
constexpr uint32_t kDrawSeam = 0x13f4e0u, kSendVuProg = 0x145e80u, kRaDestLoop = 0x13f5a0u;
constexpr uint32_t kTagWord1 = 0x202EC000u; // CreateFacePacket's template: PRE | PRIM 0x5D | NREG 2
constexpr uint32_t kTagRegs = 0x41u;        // RGBAQ, XYZF2
constexpr uint32_t kMaxBatch = 42u;

static RecompiledFunction prevDraw = nullptr, prevSend = nullptr;

// 0 off, 1 native, 2 verify (diagnostic build). DC2_G791_SHADOW=off|native|verify; unset = kDefaultMode.
// ⛔ G791 DECISION (plans/phase-G791-ledger.md §6): DEFAULT OFF. The native path is byte-exact (oracle: ~376k
// faces, 0 mismatches; final-frame and 0x139 gates at the floor) but its clean Dungeon1 price is +0.14..+0.25 ms/f
// (slower), against the >= 0.5 ms/f gain Gate 0 required. Kept compiled as an opt-in: DC2_G791_SHADOW=native.
constexpr int kDefaultMode = 0;
static int resolveMode()
{
    const char *e = std::getenv("DC2_G791_SHADOW");
    if (std::getenv("DC2_G790_SHADOW_S") || std::getenv("DC2_G790_SHADOW_R"))
        return 0; // the G790 probes own the same seam
    if (!e || !*e)
        return kDefaultMode;
    if (!std::strcmp(e, "native") || !std::strcmp(e, "1"))
        return 1;
#if defined(PS2X_G775_DIAG)
    if (!std::strcmp(e, "verify") || !std::strcmp(e, "2"))
        return 2;
#endif
    return 0;
}
static int g_mode = 0;

static inline bool ok(uint32_t a, size_t n) { return a >= 0x10000u && a < 0x02000000u && n <= 0x02000000u - a; }
static inline uint32_t rd(const uint8_t *ram, uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; }
static inline uint16_t rh(const uint8_t *ram, uint32_t a) { uint16_t v; std::memcpy(&v, ram + a, 2); return v; }
static inline uint32_t sf() { return g_dc2ScriptFrame.load(std::memory_order_relaxed); }

using V4 = std::array<float, 4>;
static inline V4 v4At(const uint8_t *p) { V4 v; std::memcpy(v.data(), p, 16); return v; }

// ---- VU float arithmetic: one IEEE single rounding per operation, exactly as the VU1 interpreter /
// native JIT (separate mul and add, never fused). Scalar SSE intrinsics so no compiler can contract them.
static inline float fmul(float a, float b) { return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float fadd(float a, float b) { return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float fsub(float a, float b) { return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float fdiv(float a, float b) { return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float fsqrt(float a) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(a))); }
static inline bool signBit(float f) { uint32_t u; std::memcpy(&u, &f, 4); return (u >> 31) != 0u; }
static inline bool finite4(const V4 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]); }

// MULAbcx / MADDAbcy / MADDAbcz / MADDbcw: r = ((m0*v.x + m1*v.y) + m2*v.z) + m3*v.w, per lane.
static inline V4 xform4(const V4 *m, const V4 &v)
{
    V4 r;
    for (int c = 0; c < 4; ++c)
    {
        float acc = fmul(m[0][c], v[0]);
        acc = fadd(acc, fmul(m[1][c], v[1]));
        acc = fadd(acc, fmul(m[2][c], v[2]));
        r[c] = fadd(acc, fmul(m[3][c], v[3]));
    }
    return r;
}
// CLIP v, v.w (judgement != 0 <=> some |x|,|y|,|z| exceeds |w|).
static inline bool clipOut(const V4 &v)
{
    const float w = std::fabs(v[3]);
    for (int c = 0; c < 3; ++c)
        if (v[c] > w || v[c] < -w)
            return true;
    return false;
}

// ---- per-draw VU state, read from the draw's renderInfo packet -----------------------------------------
struct DrawState
{
    V4 m25[4]{}, m29[4]{}, m40[4]{};
    uint8_t hdrAdd[32]{}, hdrSub[32]{};
    float l[3]{};  // vf02.x, vf03.x, vf04.x after the prologue's normalisation
    V4 vf22{};     // per-kick facing reference
};
enum Refuse : uint32_t { kROk = 0, kRPacket, kRState, kRType, kRSource, kRChain, kRNonFinite, kRAlloc, kRSize, kRRing, kRCount };
static const char *const kRefuseName[kRCount] = {"ok", "packet", "state", "type", "source", "chain", "nonfinite", "alloc", "size", "ring"};

// Validates the CreateRenderInfoPacket + SetShadowData layout and recomputes the prologue (VU 0x0006..0x0054).
static uint32_t readState(const uint8_t *ram, uint32_t ri, DrawState &s)
{
    if (!ok(ri, 55u * 16u))
        return kRPacket;
    auto w = [&](uint32_t qw, uint32_t word) { return rd(ram, ri + qw * 16u + word * 4u); };
    if (w(0, 0) != 0x10000023u || w(0, 1) || w(0, 2) || w(0, 3) || w(1, 0) || (w(1, 1) >> 24) != 0x03u ||
        (w(1, 2) >> 24) != 0x02u || w(1, 3) != 0x6C210000u || w(35, 0) || w(35, 1) || w(35, 2) || w(35, 3) != 0x14000000u ||
        w(36, 0) != 0x10000008u || w(36, 1) || w(36, 2) || w(36, 3) != 0x50000008u || w(45, 0) != 0x10000008u || w(45, 1) ||
        w(45, 2) || w(45, 3) != 0x6C080028u || w(54, 0) != 0x60000000u)
        return kRPacket;
    auto vu = [&](uint32_t a) { return v4At(ram + ri + (a < 33u ? 2u + a : 46u + (a - 40u)) * 16u); };
    for (uint32_t i = 0; i < 4; ++i)
    {
        s.m25[i] = vu(25 + i);
        s.m29[i] = vu(29 + i);
        s.m40[i] = vu(40 + i);
        if (!finite4(s.m25[i]) || !finite4(s.m29[i]) || !finite4(s.m40[i]))
            return kRNonFinite;
    }
    std::memcpy(s.hdrAdd, ram + ri + 50u * 16u, 32);
    std::memcpy(s.hdrSub, ram + ri + 52u * 16u, 32);
    // Prologue: vf02/03/04 = VU12*v.x + VU13*v.y + VU14*v.z for v = VU8 / VU9 / VU10; vf31 = sum of squares;
    // Q = 1/sqrt(|vf31.x|) (RSQRT vf00w, vf31x); MULq.x scales the three x lanes.
    const V4 m12[3] = {vu(12), vu(13), vu(14)};
    float x[3];
    for (uint32_t k = 0; k < 3; ++k)
    {
        const V4 v = vu(8 + k);
        float acc = fmul(m12[0][0], v[0]);
        acc = fadd(acc, fmul(m12[1][0], v[1]));
        x[k] = fadd(acc, fmul(m12[2][0], v[2]));
    }
    float len2 = fmul(x[0], x[0]);
    len2 = fadd(len2, fmul(x[1], x[1]));
    len2 = fadd(len2, fmul(x[2], x[2]));
    const float den = fsqrt(std::fabs(len2));
    const float q = den != 0.0f ? fdiv(1.0f, den) : std::numeric_limits<float>::max();
    for (uint32_t k = 0; k < 3; ++k)
    {
        s.l[k] = fmul(x[k], q);
        if (!std::isfinite(s.l[k]))
            return kRNonFinite;
    }
    // Per kick (0x0082..0x008D): vf21 = -0.1; vf23 = (-0.1*L); vf22 = VU25*vf23.x + VU26*vf23.y + VU27*vf23.z.
    const float m01 = -0.100000001f;
    const float a = fmul(m01, s.l[0]), b = fmul(m01, s.l[1]), c = fmul(m01, s.l[2]);
    for (int lane = 0; lane < 4; ++lane)
    {
        float acc = fmul(s.m25[0][lane], a);
        acc = fadd(acc, fmul(s.m25[1][lane], b));
        s.vf22[lane] = fadd(acc, fmul(s.m25[2][lane], c));
    }
    if (!finite4(s.vf22))
        return kRNonFinite;
    return kROk;
}

// ---- the microprogram, one source triangle (0x008E..0x0101 + the emitter 0x0109..0x018B) ----------------
struct Out
{
    std::vector<uint8_t> &bytes; // native: the ring slot's own buffer; verify: a scratch buffer
    void qw(const void *p) { const auto *b = static_cast<const uint8_t *>(p); bytes.insert(bytes.end(), b, b + 16); }
};
static bool ftoi4(const V4 &v, int32_t out[4])
{
    for (int c = 0; c < 4; ++c)
    {
        const float f = fmul(v[c], 16.0f);
        if (!(f > -2147483648.0f && f < 2147483648.0f))
            return false; // saturation / NaN: outside the proven contract
        out[c] = _mm_cvtt_ss2si(_mm_set_ss(f));
    }
    return true;
}
// Returns false when the triangle leaves the proven contract (the whole face then stays legacy).
static bool emitTriangle(const DrawState &s, const V4 src[3], Out &out, uint32_t &prisms)
{
    const V4 a = xform4(s.m40, src[0]), b = xform4(s.m40, src[1]), c = xform4(s.m40, src[2]);
    const V4 p0 = xform4(s.m25, src[0]), p1 = xform4(s.m25, src[1]), p2 = xform4(s.m25, src[2]);
    if (!finite4(a) || !finite4(b) || !finite4(c) || !finite4(p0) || !finite4(p1) || !finite4(p2))
        return false;
    if (clipOut(p0) || clipOut(p1) || clipOut(p2) || clipOut(a) || clipOut(b) || clipOut(c))
        return true; // FCAND 0x3ffff: skipped by the program, nothing emitted
    V4 e1, e2;
    for (int k = 0; k < 4; ++k) { e1[k] = fsub(p1[k], p0[k]); e2[k] = fsub(p2[k], p0[k]); }
    // OPMULA ACC, e2, e1 ; OPMSUB n, e1, e2
    const float nx = fsub(fmul(e2[1], e1[2]), fmul(e1[1], e2[2]));
    const float ny = fsub(fmul(e2[2], e1[0]), fmul(e1[2], e2[0]));
    const float nz = fsub(fmul(e2[0], e1[1]), fmul(e1[0], e2[1]));
    const float dot = fadd(fadd(fmul(nx, s.vf22[0]), fmul(ny, s.vf22[1])), fmul(nz, s.vf22[2]));
    if (!std::isfinite(dot))
        return false;
    const float eps = fmul(1.0f, 9.99999975e-05f);
    if (!(dot > eps || dot < -eps))
        return true; // FCAND 0x3 on CLIP dot, 1e-4: skipped
    const V4 *quad[5][4];
    if (signBit(dot)) // order A (0x00D3)
    {
        const V4 *qa[5][4] = {{&p0, &p1, &b, &a}, {&p1, &p2, &c, &b}, {&p2, &p0, &a, &c}, {&a, &b, &c, &c}, {&p2, &p1, &p0, &p0}};
        std::memcpy(quad, qa, sizeof(quad));
    }
    else // order B (0x00E9)
    {
        const V4 *qb[5][4] = {{&p0, &a, &b, &p1}, {&p1, &b, &c, &p2}, {&p2, &c, &a, &p0}, {&c, &b, &a, &a}, {&p0, &p1, &p2, &p2}};
        std::memcpy(quad, qb, sizeof(quad));
    }
    static const uint32_t tag[4] = {0x00008004u, kTagWord1, kTagRegs, 0u};
    static const int32_t rgba[4] = {1, 1, 1, 64};
    uint8_t subBuf[5][9 * 16];
    uint32_t nAdd = 0, nSub = 0;
    out.qw(s.hdrAdd);
    out.qw(s.hdrAdd + 16);
    for (int q = 0; q < 5; ++q)
    {
        V4 v[4];
        for (int k = 0; k < 4; ++k)
        {
            const V4 t = xform4(s.m29, *quad[q][k]);
            const float iq = t[3] != 0.0f ? fdiv(1.0f, t[3]) : std::numeric_limits<float>::max();
            for (int lane = 0; lane < 4; ++lane)
                v[k][lane] = fmul(t[lane], iq);
            if (t[3] == 0.0f || !finite4(v[k]))
                return false;
        }
        V4 f1, f2;
        for (int k = 0; k < 4; ++k) { f1[k] = fsub(v[1][k], v[0][k]); f2[k] = fsub(v[2][k], v[0][k]); }
        const float cz = fsub(fmul(f2[0], f1[1]), fmul(f1[0], f2[1]));
        if (!std::isfinite(cz))
            return false;
        int32_t iv[4][4];
        for (int k = 0; k < 4; ++k)
            if (!ftoi4(v[k], iv[k]))
                return false;
        uint8_t pkt[9 * 16];
        std::memcpy(pkt, tag, 16);
        for (int k = 0; k < 4; ++k)
        {
            std::memcpy(pkt + 16 + k * 32, rgba, 16);
            std::memcpy(pkt + 32 + k * 32, iv[k], 16);
        }
        if (signBit(cz)) // MAC Sz: buffer B, kicked after the ADD set under ALPHA 0x62
            std::memcpy(subBuf[nSub++], pkt, sizeof(pkt));
        else
        {
            out.bytes.insert(out.bytes.end(), pkt, pkt + sizeof(pkt));
            ++nAdd;
        }
    }
    if (nAdd)
    {
        out.qw(s.hdrAdd);
        out.qw(s.hdrAdd + 16);
        if (nSub)
        {
            out.qw(s.hdrSub);
            out.qw(s.hdrSub + 16);
            for (uint32_t k = 0; k < nSub; ++k)
                out.bytes.insert(out.bytes.end(), subBuf[k], subBuf[k] + sizeof(subBuf[k]));
            out.qw(s.hdrSub);
            out.qw(s.hdrSub + 16);
        }
    }
    ++prisms;
    return true;
}

// ---- source: the face's triangle list, read LIVE through its indices (exactly what CreateFacePacket copies) --
// ⛔ G790 §5's premise "static source, frozen once per DataAssignMDT" is REFUTED for part of the population. The
// first oracle run (g791v_d1_a1, frozen per visual+face) matched 96.6 % of Dungeon1 faces byte for byte; every
// mismatch was a face whose vertex array self+0x30 is rewritten IN PLACE between frames (same pointer, same
// indices, vertices moved by up to ~14 px; the same faces every frame), and `vinputBad` tracked `bad`. The
// legacy path re-reads the array every draw, so the native path does too (~600 vertex reads/f on Dungeon1).
struct FaceSrc
{
    uint32_t header = 0, count = 0;
    std::vector<V4> v; // 3 * count, in triangle-list order
};
static FaceSrc g_src;

static const FaceSrc *source(const uint8_t *ram, uint32_t self, uint32_t face, uint32_t &why)
{
    FaceSrc &f = g_src;
    f.header = rh(ram, face);
    f.count = uint32_t(int32_t(int16_t(rh(ram, face + 8))));
    const uint32_t indices = rd(ram, face + 0xc) & 0x1fffffffu, vertices = rd(ram, self + 0x30) & 0x1fffffffu;
    if ((f.header & 7u) != 3u || f.count == 0u || f.count > 4096u)
    {
        why = kRType;
        return nullptr;
    }
    if (!ok(indices, size_t(f.count) * 12u) || !ok(vertices, 16))
    {
        why = kRSource;
        return nullptr;
    }
    f.v.resize(size_t(f.count) * 3u);
    for (uint32_t i = 0; i < f.count * 3u; ++i)
    {
        const uint32_t va = vertices + rd(ram, indices + i * 4u) * 16u;
        if (!ok(va, 16))
        {
            why = kRSource;
            return nullptr;
        }
        f.v[i] = v4At(ram + va);
        if (!finite4(f.v[i]))
        {
            why = kRNonFinite;
            return nullptr;
        }
    }
    return &f;
}

// ---- census (counters always; printed only with DC2_G791_STATS=1) -------------------------------------
struct Census
{
    uint64_t draws = 0, buckets = 0, faces = 0, native = 0, tris = 0, prisms = 0, qw = 0, refused[kRCount]{};
    uint64_t vFaces = 0, vInputBad = 0;
    uint32_t lastSf = 0;
};
static Census g_c;
static bool statsOn()
{
    static const bool v = [] { const char *e = std::getenv("DC2_G791_STATS"); return e && *e && std::strcmp(e, "0") != 0; }();
    return v;
}
static void report()
{
    if (!statsOn() || sf() < g_c.lastSf + 600u)
        return;
    g_c.lastSf = sf();
    char r[256];
    int at = 0;
    for (uint32_t k = 1; k < kRCount && at < 230; ++k)
        at += std::snprintf(r + at, sizeof(r) - size_t(at), " %s=%llu", kRefuseName[k], (unsigned long long)g_c.refused[k]);
    std::fprintf(stderr, "[G791:shadow] sf=%u mode=%d draws=%llu buckets=%llu faces=%llu native=%llu tris=%llu prisms=%llu "
                         "qw=%llu vfaces=%llu vinputBad=%llu gs(markers=%llu parsed=%llu orphans=%llu) | refused:%s\n",
                 sf(), g_mode, (unsigned long long)g_c.draws, (unsigned long long)g_c.buckets, (unsigned long long)g_c.faces,
                 (unsigned long long)g_c.native, (unsigned long long)g_c.tris, (unsigned long long)g_c.prisms,
                 (unsigned long long)g_c.qw, (unsigned long long)g_c.vFaces, (unsigned long long)g_c.vInputBad,
                 (unsigned long long)g_g791C.markers.load(std::memory_order_relaxed),
                 (unsigned long long)g_g791C.parsed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g791C.orphans.load(std::memory_order_relaxed), r);
}

// ---- per draw (EE thread) ------------------------------------------------------------------------------
struct Pending
{
    uint32_t self = 0, frame = 0, memory = 0, renderInfo = 0;
    bool active = false, parsed = false;
    uint32_t stateWhy = kROk;
    DrawState state;
};
static Pending g_pending;
static std::vector<uint8_t> g_scratch; // verify mode
static uint32_t g_seq = 0;

static uint32_t allocQw(uint8_t *ram, uint32_t memory, uint32_t qw)
{
    if (rd(ram, memory + 0x1c) != 0u)
        return 0u;
    const uint32_t used = rd(ram, memory + 0x24), cap = rd(ram, memory + 0x28);
    if (used + qw >= cap)
        return 0u;
    const uint32_t at = (rd(ram, memory + 0x20) + used * 16u) & 0x1fffffffu;
    if (!ok(at, size_t(qw) * 16))
        return 0u;
    const uint32_t nu = used + qw;
    std::memcpy(ram + memory + 0x24, &nu, 4);
    return at;
}

// [VIF NOP NOP NOP DIRECT 1] + a payload-less GIF tag (NLOOP 0, EOP, PACKED, NREG 1) whose REGS carry the face.
static void marker(uint8_t *ram, uint32_t at, uint32_t kind, uint32_t seq)
{
    const uint32_t vif[4] = {0u, 0u, 0u, 0x50000001u};
    std::memcpy(ram + at, vif, 16);
    const uint64_t lo = 0x8000ull | (1ull << 60);
    const uint64_t hi = (kG791MarkerMagic << 48) | (uint64_t(kind) << 40) | seq;
    std::memcpy(ram + at + 16, &lo, 8);
    std::memcpy(ram + at + 24, &hi, 8);
}

#if defined(PS2X_G775_DIAG)
// Verify: the face packet's own input must be the source read here (tag template, header, every vertex).
static bool inputMatches(const uint8_t *ram, uint32_t pkt, uint32_t qwc, const FaceSrc &f)
{
    uint32_t q = 0, done = 0;
    while (done < f.count)
    {
        const uint32_t n = std::min(kMaxBatch, f.count - done);
        if (q + 4u + 3u * n > qwc)
            return false;
        const uint32_t at = pkt + q * 16u;
        if (rd(ram, at) || rd(ram, at + 4) || rd(ram, at + 8) || rd(ram, at + 12) != (0x6C008000u | ((2u + 3u * n) << 16)))
            return false;
        if (rd(ram, at + 16) != (0x8000u | n) || rd(ram, at + 20) != kTagWord1 || rd(ram, at + 24) != kTagRegs || rd(ram, at + 28))
            return false;
        if (rd(ram, at + 32) != n || rd(ram, at + 36) != n || rd(ram, at + 40) != f.header)
            return false;
        if (std::memcmp(ram + at + 48, f.v.data() + size_t(done) * 3u, size_t(n) * 48u) != 0)
            return false;
        const uint32_t mscal = at + (3u + 3u * n) * 16u;
        if (rd(ram, mscal) || rd(ram, mscal + 4) || rd(ram, mscal + 8) || rd(ram, mscal + 12) != 0x14000002u)
            return false;
        q += 4u + 3u * n;
        done += n;
    }
    return q + 1u == qwc && rd(ram, pkt + q * 16u) == 0x13000000u;
}
#endif

static void bucket(uint8_t *ram, uint32_t bucketAddr)
{
    ++g_c.buckets;
    Pending &p = g_pending;
    if (!p.parsed)
    {
        p.parsed = true;
        p.stateWhy = readState(ram, p.renderInfo, p.state);
    }
    const uint32_t chain = ok(bucketAddr, 0x20) ? rd(ram, bucketAddr + 0x10) & 0x1fffffffu : 0u;
    const uint32_t chainQw = ok(bucketAddr, 0x20) ? rd(ram, bucketAddr + 0x14) : 0u;
    std::array<uint32_t, 64> refs{}, faces{};
    uint32_t nRef = 0, nFace = 0;
    bool good = chain && chainQw > 0u && chainQw < 4096u && ok(chain, size_t(chainQw) * 16);
    if (good)
    {
        for (uint32_t face = rd(ram, bucketAddr + 4) & 0x1fffffffu; face; face = rd(ram, face + 0x10) & 0x1fffffffu)
        {
            if (!ok(face, 0x20) || nFace == faces.size()) { good = false; break; }
            faces[nFace++] = face;
        }
        for (uint32_t q = 0; good && q < chainQw;)
        {
            const uint32_t tag = rd(ram, chain + q * 16);
            const uint32_t id = (tag >> 28) & 7u, qwc = tag & 0xffffu;
            if (id == 3u) { if (nRef == refs.size()) good = false; else refs[nRef++] = chain + q * 16; ++q; }
            else if (id == 1u) q += 1u + qwc;
            else if (id == 6u) break;
            else good = false;
        }
        good = good && nRef == nFace;
    }
    g_c.faces += nFace;
    if (!good || p.stateWhy != kROk)
    {
        g_c.refused[good ? p.stateWhy : uint32_t(kRChain)] += nFace;
        return;
    }
    for (uint32_t k = 0; k < nFace; ++k)
    {
        uint32_t why = kROk;
        const FaceSrc *f = source(ram, p.self, faces[k], why);
        const uint32_t ref = refs[k];
        const uint32_t refQwc = rd(ram, ref) & 0xffffu, refAddr = rd(ram, ref + 4) & 0x1fffffffu;
        if (f)
        {
            const uint32_t batches = (f->count + kMaxBatch - 1u) / kMaxBatch;
            if (refQwc != 4u * batches + 3u * f->count + 1u || !ok(refAddr, size_t(refQwc) * 16))
                why = kRChain;
        }
        if (why == kROk)
        {
            // Native: the bytes are generated straight into a free host ring slot (each slot keeps its own
            // buffer, so memory is bounded by kG791Ring x the largest face); the chain carries a 2-qword marker.
            // ⛔ The first ring build SWAPPED one EE scratch vector through the slots: every buffer drifted
            // towards the largest face's capacity (~150 KB) across 8,192 slots — private bytes +7.8 MB/s,
            // 0.95 -> 3.0 GB in 140 s and the frame 16.5 -> 26 ms/f (g791x_d1_bmem1 vs legacy flat 1.79 GB).
            G791Slot *slot = nullptr;
            uint32_t seq = 0;
            std::vector<uint8_t> *dst = &g_scratch;
#if defined(PS2X_G775_DIAG)
            if (g_mode != 2)
#endif
            {
                seq = ++g_seq;
                if (seq == 0u)
                    seq = g_seq = 1u;
                slot = &g_g791Ring[seq & (kG791Ring - 1u)];
                if (slot->state.load(std::memory_order_acquire) != 0u)
                    why = kRRing;
                else
                    dst = &slot->bytes;
            }
            dst->clear();
            Out out{*dst};
            uint32_t prisms = 0;
            for (uint32_t t = 0; t < f->count && why == kROk; ++t)
                if (!emitTriangle(p.state, f->v.data() + size_t(t) * 3u, out, prisms))
                    why = kRNonFinite;
            if (why == kROk && dst->size() / 16u + 1u > 0xffffu)
                why = kRSize;
            if (why == kROk)
            {
                const uint32_t n = uint32_t(dst->size() / 16u);
#if defined(PS2X_G775_DIAG)
                if (g_mode == 2)
                {
                    ++g_c.vFaces;
                    if (!inputMatches(ram, refAddr, refQwc, *f))
                        ++g_c.vInputBad;
                    seq = ++g_seq;
                    if (seq == 0u) seq = g_seq = 1u;
                    const uint32_t blob = allocQw(ram, p.memory, refQwc + 4u);
                    if (!blob)
                        why = kRAlloc;
                    else
                    {
                        ::dc2_g791_expect(seq, dst->data(), dst->size());
                        marker(ram, blob, kG791KindBegin, seq);
                        std::memmove(ram + blob + 32, ram + refAddr, size_t(refQwc) * 16);
                        marker(ram, blob + 32 + refQwc * 16u, kG791KindEnd, seq);
                        const uint64_t lo = uint64_t(refQwc + 4u) | (3ull << 28) | (uint64_t(blob) << 32);
                        std::memcpy(ram + ref, &lo, 8);
                        g_c.tris += f->count;
                        g_c.prisms += prisms;
                        g_c.qw += n;
                    }
                }
                else
#endif
                {
                    const uint32_t blob = allocQw(ram, p.memory, 2u);
                    if (!blob)
                        why = kRAlloc;
                    else
                    {
                        slot->seq.store(seq, std::memory_order_relaxed);
                        slot->state.store(1u, std::memory_order_release);
                        marker(ram, blob, kG791KindNative, seq);
                        const uint64_t lo = 2ull | (3ull << 28) | (uint64_t(blob) << 32);
                        std::memcpy(ram + ref, &lo, 8);
                        ++g_c.native;
                        g_c.tris += f->count;
                        g_c.prisms += prisms;
                        g_c.qw += n;
                    }
                }
            }
        }
        if (why != kROk)
            ++g_c.refused[why];
    }
}

// ---- seams --------------------------------------------------------------------------------------------
static void drawWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
    if (ok(self, 0x50) && rd(ram, self + 0x1c) == kShadowVtable && getRegU32(ctx, 5) != 0u)
    {
        report();
        ++g_c.draws;
        const uint32_t manager = getRegU32(ctx, 7) ? getRegU32(ctx, 7) & 0x1fffffffu : 0x003820e0u;
        const uint32_t memory = ok(manager, 0x70) ? rd(ram, manager + 0x60) & 0x1fffffffu : 0u;
        g_pending = Pending{};
        if (ok(memory, 0x30))
        {
            g_pending.self = self;
            g_pending.frame = sf();
            g_pending.memory = memory;
            g_pending.renderInfo = (rd(ram, memory + 0x20) + rd(ram, memory + 0x24) * 16u) & 0x1fffffffu;
            g_pending.active = true;
        }
    }
    else if (g_pending.active)
        g_pending.active = false;
    prevDraw(ram, ctx, runtime);
}
static void sendWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_pending.active && getRegU32(ctx, 31) == kRaDestLoop && g_pending.frame == sf())
        bucket(ram, getRegU32(ctx, 16) & 0x1fffffffu); // Draw's s0 = the bucket
    prevSend(ram, ctx, runtime);
}
} // namespace g791

static void g791_register(PS2Runtime &runtime)
{
    g791::g_mode = g791::resolveMode();
    if (g791::g_mode == 0)
        return;
    auto hook = [&](uint32_t addr, g791::RecompiledFunction &prev, g791::RecompiledFunction w) {
        prev = runtime.lookupFunction(addr);
        if (!prev) { std::fprintf(stderr, "[G791:shadow] FATAL no function at %x\n", addr); std::abort(); }
        runtime.registerFunction(addr, w);
    };
    hook(g791::kDrawSeam, g791::prevDraw, &g791::drawWrapper);
    hook(g791::kSendVuProg, g791::prevSend, &g791::sendWrapper);
    std::fprintf(stderr, "[G791:shadow] armed mode=%s\n", g791::g_mode == 2 ? "verify" : "native");
}
