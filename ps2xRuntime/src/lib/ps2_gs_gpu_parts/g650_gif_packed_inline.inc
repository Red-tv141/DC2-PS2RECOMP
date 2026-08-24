// ============================================================================================
// G650 (ROADMAP Priority 3) — COMPILED GIF/GS FRONT-END: the PACKED descriptor bodies, inlined
// ============================================================================================
//
// PREMISE. `[G147:gif] parserOther` (= packet − draw − image) is ~9 ms/f on the GS worker — the
// POLE thread — across ~141 k PACKED descriptors/frame on `s05` and ~88.5 k on `dragon`. G602
// already deleted the per-descriptor bounds test and the staging `regs[16]` array, so what is left
// in the walk is, per descriptor:
//
//   1. an out-of-line CALL to `GS::writeRegisterPacked` — 17d §2.1's measured call-boundary class
//      (G487: ~10 ns/call, and this callee is big enough that MSVC will not inline it), and
//   2. a 16-way `switch` dispatch inside it, re-selected for every descriptor even though a GIF
//      tag's descriptor list is FIXED for the whole tag.
//
// ⭐ THE MECHANISM. The five descriptors that carry vertex data (`RGBAQ` 0x01, `ST` 0x02, `UV` 0x03,
// `XYZF2` 0x04, `XYZ2` 0x05) plus `FOG` 0x0A and `NOP` 0x0F are ~all of the traffic on a geometry
// route. Their bodies move into MACROS here, and BOTH consumers expand the same macro:
//
//   * `GS::writeRegisterPacked`'s own `case` labels — so the interpretive path is byte-identical
//     to what it was, and
//   * a new inline dispatch inside `processGIFPacket`'s PACKED walk, which therefore executes the
//     body WITHOUT the call and without re-entering the callee's switch.
//
// Anything outside that set (A+D 0x0E, and every other descriptor) still takes the original call,
// so coverage is a performance property and never a correctness one.
//
// ⛔ EXACTNESS IS BY CONSTRUCTION, AND IT IS THE MACRO THAT MAKES IT SO. There is no second copy
// of the arithmetic to drift: `writeRegisterPacked` and the inline walk expand the same tokens.
// That includes the parts that are easy to get wrong and that a re-implementation would lose:
//   * the G52 zero-RGBAQ HOLD (`G602_GATE_PK_G52HOLD`, which is ON by default),
//   * `m_curQ == 0.0f ⇒ 1.0f` in ST,
//   * `XYZF2` taking its fog from `hi>>36` while `XYZ2` takes `m_curFog`,
//   * the G105 synthetic-ADC guard and its `g105Restart ⇒ m_vtxCount = 0` postcondition,
//   * `G602_GATE_PK_FORCEDRAW` clearing `adk`.
//
// ⚠️ SINGLE-FORM PER ARM — G645's law. The rollback is NOT an in-loop ternary; it selects a
// DUPLICATED loop, which is the same remedy G593 and G602 applied in this very function for the
// same measured reason (a loop-invariant branch inside the 141 k-descriptor loop cost +0.64 ms/f).
//
// Rollback: DC2_G650_NO_GIFINLINE=1     Census: DC2_G650_GIF_STAT=1
// ============================================================================================

#ifndef DC2_G650_GIF_PACKED_INLINE_INC
#define DC2_G650_GIF_PACKED_INLINE_INC 1

namespace
{

// ⛔ DEFAULT-OFF. The mechanism lands — `[G650:gif]` reports 90.15% of 364 M PACKED descriptors
// inlined on `dungeon1`, 91.76% of 204 M on `dragon` — and the payoff is NULL:
//   A B B A on `dungeon1`: frame blocks −0.373 / +0.445 (pooled +0.036), GS own −0.372 / +0.419.
//   And the bucket the lever TOUCHES does not move: `[G147:gif] parserOther` reads 3.83 / 3.88 /
//   3.83 / 3.92 ms/f across an off/on/on/off battery — a −0.02 ms/f difference on a 3.9 ms/f bucket.
// That is G647's law in a second layer: deleting a CALL is not deleting WORK. `parserOther` is
// ~40 ns/descriptor here and the call boundary is a small fraction of it, which the inline form
// pays straight back in `processGIFPacket`'s text (17d's inline-budget cliff — the function now
// carries two vertex-kick bodies).
// Retained as its own probe: `DC2_G650_GIFINLINE=1` arms it, `DC2_G650_NO_GIFINLINE=1` still wins.
bool g650GifInlineOn()
{
    static const bool s_on = (std::getenv("DC2_G650_GIFINLINE") != nullptr) &&
                             (std::getenv("DC2_G650_NO_GIFINLINE") == nullptr);
    return s_on;
}

bool g650GifStatOn()
{
    static const bool s_on = (std::getenv("DC2_G650_GIF_STAT") != nullptr);
    return s_on;
}

bool g652Gif412On()
{
    const int arm = g652Gif412Arm();
    if (arm >= 0)
        return arm != 0;
    static const bool s_on = std::getenv("DC2_G652_GIF412") != nullptr &&
                             std::getenv("DC2_G652_NO_GIF412") == nullptr;
    return s_on;
}

// Behaviour-pure census: how much of the PACKED descriptor stream the inline dispatch actually
// covers, and what the residue is. Read only under DC2_G650_GIF_STAT.
std::atomic<uint64_t> g_g650GifDesc{0ull};
std::atomic<uint64_t> g_g650GifInlined{0ull};
std::atomic<uint64_t> g_g650GifCalled{0ull};
std::atomic<uint64_t> g_g650GifByDesc[16] = {};

void g650GifReport()
{
    if (!g650GifStatOn())
        return;
    const uint64_t n = g_g650GifDesc.load(std::memory_order_relaxed);
    if (n == 0ull || (n % 4000000ull) != 0ull)
        return;
    const uint64_t inl = g_g650GifInlined.load(std::memory_order_relaxed);
    const uint64_t call = g_g650GifCalled.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[G650:gif] desc=%llu inlined=%llu (%.2f%%) called=%llu (%.2f%%) byDesc:",
                 (unsigned long long)n, (unsigned long long)inl,
                 n ? 100.0 * (double)inl / (double)n : 0.0,
                 (unsigned long long)call, n ? 100.0 * (double)call / (double)n : 0.0);
    for (int d = 0; d < 16; ++d)
    {
        const uint64_t c = g_g650GifByDesc[d].load(std::memory_order_relaxed);
        if (c)
            std::fprintf(stderr, " %X=%.2f%%", d, 100.0 * (double)c / (double)n);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace

// ============================================================================================
// The descriptor bodies. Each expands inside a `GS` member function, so `m_cur*` / `m_ctx` /
// `m_vtxQueue` / `vertexKick` resolve exactly as they did when the body was a `case` label.
// ============================================================================================

// 0x01 RGBAQ. The G52 hold is part of the body, not a wrapper around it.
#define G650_PK_RGBAQ(lo_, hi_)                                                                    \
    do                                                                                             \
    {                                                                                              \
        if (!(G602_GATE_PK_G52HOLD && ((lo_) == 0u && (hi_) == 0u) &&                               \
              m_prim.type == GS_PRIM_TRISTRIP &&                                                    \
              m_ctx[m_prim.ctxt & 1u].frame.fbp == 0x139u))                                         \
        {                                                                                          \
            m_curR = static_cast<uint8_t>((lo_) & 0xFF);                                            \
            m_curG = static_cast<uint8_t>(((lo_) >> 32) & 0xFF);                                    \
            m_curB = static_cast<uint8_t>((hi_) & 0xFF);                                            \
            m_curA = static_cast<uint8_t>(((hi_) >> 32) & 0xFF);                                    \
        }                                                                                          \
        if (G602_GATE_PK_RGBAQTRACE)                                                                \
            g602ColdPackedRgbaqTrace(static_cast<uint32_t>(m_prim.type),                             \
                                     m_ctx[m_prim.ctxt & 1u].frame.fbp,                              \
                                     m_curR, m_curG, m_curB, m_curA, (lo_), (hi_));                  \
    } while (0)

// 0x02 ST (+Q).
#define G650_PK_ST(lo_, hi_)                                                                       \
    do                                                                                             \
    {                                                                                              \
        const uint32_t g650sBits = static_cast<uint32_t>((lo_) & 0xFFFFFFFF);                        \
        const uint32_t g650tBits = static_cast<uint32_t>(((lo_) >> 32) & 0xFFFFFFFF);                \
        const uint32_t g650qBits = static_cast<uint32_t>((hi_) & 0xFFFFFFFF);                        \
        std::memcpy(&m_curS, &g650sBits, 4);                                                        \
        std::memcpy(&m_curT, &g650tBits, 4);                                                        \
        std::memcpy(&m_curQ, &g650qBits, 4);                                                        \
        if (m_curQ == 0.0f)                                                                         \
            m_curQ = 1.0f;                                                                          \
    } while (0)

// 0x03 UV.
#define G650_PK_UV(lo_)                                                                            \
    do                                                                                             \
    {                                                                                              \
        m_curU = static_cast<uint16_t>((lo_) & 0xFFFFu);                                            \
        m_curV = static_cast<uint16_t>(((lo_) >> 32) & 0xFFFFu);                                    \
    } while (0)

// 0x04 XYZF2 — the drawing kick. Fog comes from the descriptor, not from `m_curFog`.
#define G650_PK_XYZF2(lo_, hi_)                                                                    \
    do                                                                                             \
    {                                                                                              \
        const uint16_t g650x = static_cast<uint16_t>((lo_) & 0xFFFF);                                \
        const uint16_t g650y = static_cast<uint16_t>(((lo_) >> 32) & 0xFFFF);                        \
        const uint32_t g650z = static_cast<uint32_t>(((hi_) >> 4) & 0xFFFFFF);                       \
        const uint8_t g650f = static_cast<uint8_t>(((hi_) >> 36) & 0xFF);                            \
        bool g650adk = (((hi_) >> 47) & 1) != 0;                                                     \
        if (G602_GATE_PK_FORCEDRAW)                                                                 \
            g650adk = false;                                                                        \
        bool g650Restart = false;                                                                   \
        if (!g650adk && G602_GATE_PK_SYNTHADC &&                                                     \
            g602ColdPackedSynthAdc(g650x, g650y, m_ctx[m_prim.ctxt & 1u],                            \
                                   static_cast<uint32_t>(m_prim.type), m_prim.tme, g650Restart))     \
        {                                                                                          \
            g650adk = true;                                                                         \
        }                                                                                          \
        if (G602_GATE_PK_XYZTRACE)                                                                  \
            g602ColdPackedXyzTrace(static_cast<uint32_t>(m_prim.type), m_ctx[m_prim.ctxt & 1u],      \
                                   m_curR, m_curG, m_curB, m_curA, m_vtxCount, g650adk,              \
                                   g650x, g650y, g650z, g650f, (lo_), (hi_), kMaxVerts);             \
        GSVertex &g650vtx = m_vtxQueue[m_vtxCount % kMaxVerts];                                      \
        g650vtx.x = static_cast<float>(g650x) / 16.0f;                                               \
        g650vtx.y = static_cast<float>(g650y) / 16.0f;                                               \
        g650vtx.z = static_cast<float>(g650z);                                                       \
        g650vtx.r = m_curR;                                                                          \
        g650vtx.g = m_curG;                                                                          \
        g650vtx.b = m_curB;                                                                          \
        g650vtx.a = m_curA;                                                                          \
        g650vtx.q = m_curQ;                                                                          \
        g650vtx.s = m_curS;                                                                          \
        g650vtx.t = m_curT;                                                                          \
        g650vtx.u = m_curU;                                                                          \
        g650vtx.v = m_curV;                                                                          \
        g650vtx.fog = g650f;                                                                         \
        vertexKick(!g650adk);                                                                        \
        if (g650Restart)                                                                             \
            m_vtxCount = 0;                                                                          \
    } while (0)

// 0x05 XYZ2 — same kick, 32-bit Z, fog held in `m_curFog`.
#define G650_PK_XYZ2(lo_, hi_)                                                                     \
    do                                                                                             \
    {                                                                                              \
        const uint16_t g650x = static_cast<uint16_t>((lo_) & 0xFFFF);                                \
        const uint16_t g650y = static_cast<uint16_t>(((lo_) >> 32) & 0xFFFF);                        \
        const uint32_t g650z = static_cast<uint32_t>((hi_) & 0xFFFFFFFF);                            \
        bool g650adk = (((hi_) >> 47) & 1) != 0;                                                     \
        if (G602_GATE_PK_FORCEDRAW)                                                                 \
            g650adk = false;                                                                        \
        bool g650Restart = false;                                                                   \
        if (!g650adk && G602_GATE_PK_SYNTHADC &&                                                     \
            g602ColdPackedSynthAdc(g650x, g650y, m_ctx[m_prim.ctxt & 1u],                            \
                                   static_cast<uint32_t>(m_prim.type), m_prim.tme, g650Restart))     \
        {                                                                                          \
            g650adk = true;                                                                         \
        }                                                                                          \
        GSVertex &g650vtx = m_vtxQueue[m_vtxCount % kMaxVerts];                                      \
        g650vtx.x = static_cast<float>(g650x) / 16.0f;                                               \
        g650vtx.y = static_cast<float>(g650y) / 16.0f;                                               \
        g650vtx.z = static_cast<float>(g650z);                                                       \
        g650vtx.r = m_curR;                                                                          \
        g650vtx.g = m_curG;                                                                          \
        g650vtx.b = m_curB;                                                                          \
        g650vtx.a = m_curA;                                                                          \
        g650vtx.q = m_curQ;                                                                          \
        g650vtx.s = m_curS;                                                                          \
        g650vtx.t = m_curT;                                                                          \
        g650vtx.u = m_curU;                                                                          \
        g650vtx.v = m_curV;                                                                          \
        g650vtx.fog = m_curFog;                                                                      \
        vertexKick(!g650adk);                                                                        \
        if (g650Restart)                                                                             \
            m_vtxCount = 0;                                                                          \
    } while (0)

// 0x0A FOG.
#define G650_PK_FOG(hi_) (m_curFog = static_cast<uint8_t>(((hi_) >> 36) & 0xFF))

// The inline dispatch used by `processGIFPacket`'s walk. Descriptors outside the covered set take
// the original out-of-line call, so this is complete by construction.
#define G650_PK_DISPATCH(d_, lo_, hi_)                                                             \
    switch (d_)                                                                                    \
    {                                                                                              \
    case 0x01u: G650_PK_RGBAQ((lo_), (hi_)); break;                                                \
    case 0x02u: G650_PK_ST((lo_), (hi_)); break;                                                   \
    case 0x03u: G650_PK_UV((lo_)); break;                                                          \
    case 0x04u: G650_PK_XYZF2((lo_), (hi_)); break;                                                \
    case 0x05u: G650_PK_XYZ2((lo_), (hi_)); break;                                                 \
    case 0x0Au: G650_PK_FOG((hi_)); break;                                                         \
    case 0x0Fu: break;                                                                             \
    default: writeRegisterPacked((d_), (lo_), (hi_)); break;                                       \
    }

#endif // DC2_G650_GIF_PACKED_INLINE_INC
