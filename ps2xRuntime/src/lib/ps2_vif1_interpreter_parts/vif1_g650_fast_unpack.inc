// ============================================================================================
// G650 (ROADMAP Priority 4) — COMPILED VIF1 UNPACK KERNELS
// ============================================================================================
//
// PREMISE. `PS2Memory::processVIF1Data` is 12.5–13.5% of the EE thread (`[G483:rawpc]`, G645 §5.2)
// and the bulk of it is the per-WRITE-VECTOR loop in `vif1_unpack_engine.inc`. G649 P4 already
// hoisted the MASK extraction and unrolled the four-lane write. What is LEFT in that loop is a
// per-vector re-derivation of quantities that are **loop-invariant for the whole UNPACK command**:
//
//   * `vl` / `vn` / `components` select the decode shape — an if/else chain of four `vl` tests and
//     a `components == 1` test, re-walked once per vector;
//   * a 16-byte LOAD of the DESTINATION (`memcpy(lanes, m_vu1Data + destOff, 16)`) that exists only
//     so a masked or partial-width lane can keep its old value. For V4 and S formats every lane is
//     written, so that load is pure waste — and it is a read of VU data RAM the store is about to
//     overwrite, i.e. a store-forward hazard on the next vector at the same address;
//   * four `g649Lane` evaluations, each re-testing `decoded` / `handledFormat` / `maskSpec`;
//   * one `g297NoteVuWrite` call per vector, where the MTVU recorder's own G482 coalescer will
//     immediately re-merge them into one contiguous record.
//
// ⭐ THE SHAPE. Every one of those decisions is a function of values fixed before the loop starts.
// So the arm is selected ONCE, OUTSIDE the loop, and each arm is a single-form tight kernel with no
// residual test in its body — G645's law (an in-loop rollback ternary keeps both forms live and
// measured +0.354 ms/f). In the dominant case the whole command collapses to ONE `memcpy`.
//
// ⛔ ADMISSION (all loop-invariant, all checked at the call site in `vif1_unpack_engine.inc`):
//   1. `!maskEnable`                 ⇒ every `maskSpec` is 0.
//   2. `(vif1_regs.mode & 3) == 0`   ⇒ no row accumulate, no row write-back.
//   3. `cl == wl`                    ⇒ `g645Blocked` is true, `sourceAvailable` is always true, and
//                                      `destVec` is exactly `vuAddr + writeIndex` — the write run is
//                                      CONTIGUOUS. (`cl > wl` skips destination vectors; `cl < wl`
//                                      skips SOURCE vectors. Both are excluded.)
//   4. `vuAddr + writeVectorCount <= 0x400` ⇒ no `& 0x3FF` wrap, and since
//      `PS2_VU1_DATA_SIZE == 0x400 * 16` the engine's `destOff + 16 > PS2_VU1_DATA_SIZE` guard is
//      the identity over the whole run.
//   5. the format is one the engine HANDLES (`vl != 3 || vn == 3`) ⇒ `handledFormat` is true, so
//      the legacy raw-copy fall-through can never be taken.
//   6. `!g137Active` ⇒ no per-vector origin recording.
//
// Under 1–6 the engine's body reduces, for every vector `i`, to: decode source vector `i` into four
// words and store them at `m_vu1Data + (vuAddr + i) * 16`, with the lanes that the decode does not
// cover (`components < 4`, and only then) keeping the destination's previous contents. That is
// exactly what the kernels below do, one kernel per decode shape.
//
// EXACTNESS. `DC2_G650_FASTUNPACK_VERIFY=1` snapshots the destination run, executes the kernel,
// saves its result, RESTORES the snapshot and then runs the shipped legacy loop over the identical
// pre-state, comparing the two byte for byte (`[G650:unpack] bad=`). It is not a re-implementation
// of the comparison: the legacy loop IS the oracle.
//
// Rollback: DC2_G650_NO_FASTUNPACK=1   Census: DC2_G650_UNPACK_STAT=1
// ============================================================================================

#ifndef DC2_G650_FAST_UNPACK_INC
#define DC2_G650_FAST_UNPACK_INC 1

namespace
{

bool g650FastUnpackOn()
{
    static const bool s_off = (std::getenv("DC2_G650_NO_FASTUNPACK") != nullptr);
    return !s_off;
}

bool g650FastUnpackVerify()
{
    static const bool s_on = (std::getenv("DC2_G650_FASTUNPACK_VERIFY") != nullptr);
    return s_on;
}

bool g650UnpackStatOn()
{
    static const bool s_on = (std::getenv("DC2_G650_UNPACK_STAT") != nullptr);
    return s_on;
}

// Census counters. Behaviour-pure; read only under DC2_G650_UNPACK_STAT.
std::atomic<uint64_t> g_g650UnpackCmds{0ull};
std::atomic<uint64_t> g_g650UnpackFastCmds{0ull};
std::atomic<uint64_t> g_g650UnpackVecs{0ull};
std::atomic<uint64_t> g_g650UnpackFastVecs{0ull};
std::atomic<uint64_t> g_g650UnpackMemcpyVecs{0ull}; // the V4-32 identity arm
std::atomic<uint64_t> g_g650UnpackBad{0ull};
std::atomic<uint64_t> g_g650UnpackChecks{0ull};
// Reject histogram, so a route that never takes the fast path says WHY in one run.
std::atomic<uint64_t> g_g650UnpackRej[6] = {}; // mask, mode, cycle, wrap, format, trace

inline void g650RejBump(int which)
{
    if (g650UnpackStatOn() && which >= 0 && which < 6)
        g_g650UnpackRej[which].fetch_add(1ull, std::memory_order_relaxed);
}

// The decode shape. Selected once per command; each value is one kernel below.
enum G650UnpackKind
{
    G650_UK_NONE = 0,
    G650_UK_V4_32,   // vn=3 vl=0 : 16 B -> 16 B, the identity
    G650_UK_V4_16,   // vn=3 vl=1 :  8 B -> 4 x sext/zext16
    G650_UK_V4_8,    // vn=3 vl=2 :  4 B -> 4 x sext/zext8
    G650_UK_V4_5,    // vn=3 vl=3 :  2 B -> 5:5:5:1
    G650_UK_S_32,    // vn=0 vl=0 :  4 B broadcast
    G650_UK_S_16,    // vn=0 vl=1 :  2 B broadcast
    G650_UK_S_8,     // vn=0 vl=2 :  1 B broadcast
    G650_UK_PART_32, // vn=1/2 vl=0 : write `components` words, keep the rest
    G650_UK_PART_16, // vn=1/2 vl=1
    G650_UK_PART_8   // vn=1/2 vl=2
};

G650UnpackKind g650SelectUnpackKind(uint8_t vn, uint8_t vl)
{
    if (vn == 3u)
    {
        switch (vl)
        {
        case 0u: return G650_UK_V4_32;
        case 1u: return G650_UK_V4_16;
        case 2u: return G650_UK_V4_8;
        default: return G650_UK_V4_5;
        }
    }
    if (vn == 0u)
    {
        switch (vl)
        {
        case 0u: return G650_UK_S_32;
        case 1u: return G650_UK_S_16;
        case 2u: return G650_UK_S_8;
        default: return G650_UK_NONE; // vl==3 && vn!=3 is an unhandled format
        }
    }
    // vn == 1 (V2) or vn == 2 (V3): a partial-width write that must preserve the upper lanes.
    switch (vl)
    {
    case 0u: return G650_UK_PART_32;
    case 1u: return G650_UK_PART_16;
    case 2u: return G650_UK_PART_8;
    default: return G650_UK_NONE;
    }
}

// ⭐ THE KERNELS. One tight loop per decode shape, arm already selected. `dst` is
// `m_vu1Data + vuAddr * 16` and the run is contiguous, so the destination cursor is a plain
// pointer bump — no `& 0x3FF`, no bounds test, no cycle arithmetic.
void g650RunUnpackKernel(G650UnpackKind kind, uint8_t *dst, const uint8_t *src,
                         uint32_t n, uint32_t bytesPerVector, uint32_t components,
                         bool zeroExtend)
{
    switch (kind)
    {
    case G650_UK_V4_32:
        // The identity. `bytesPerVector` is 16 and every lane is written from the source, so the
        // entire command is one contiguous copy.
        std::memcpy(dst, src, static_cast<size_t>(n) * 16u);
        break;

    case G650_UK_V4_16:
        if (zeroExtend)
        {
            for (uint32_t i = 0; i < n; ++i, dst += 16, src += 8)
            {
                uint16_t raw[4];
                std::memcpy(raw, src, 8);
                const uint32_t w[4] = {raw[0], raw[1], raw[2], raw[3]};
                std::memcpy(dst, w, 16);
            }
        }
        else
        {
            for (uint32_t i = 0; i < n; ++i, dst += 16, src += 8)
            {
                int16_t raw[4];
                std::memcpy(raw, src, 8);
                const int32_t w[4] = {raw[0], raw[1], raw[2], raw[3]};
                std::memcpy(dst, w, 16);
            }
        }
        break;

    case G650_UK_V4_8:
        if (zeroExtend)
        {
            for (uint32_t i = 0; i < n; ++i, dst += 16, src += 4)
            {
                const uint32_t w[4] = {src[0], src[1], src[2], src[3]};
                std::memcpy(dst, w, 16);
            }
        }
        else
        {
            for (uint32_t i = 0; i < n; ++i, dst += 16, src += 4)
            {
                const int32_t w[4] = {static_cast<int8_t>(src[0]), static_cast<int8_t>(src[1]),
                                      static_cast<int8_t>(src[2]), static_cast<int8_t>(src[3])};
                std::memcpy(dst, w, 16);
            }
        }
        break;

    case G650_UK_V4_5:
        // V4-5 is unsigned by construction in the engine (it masks bit fields out of one u16),
        // so `zeroExtend` does not participate — exactly as in the legacy body.
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            uint16_t packed = 0u;
            std::memcpy(&packed, src, 2);
            const uint32_t w[4] = {static_cast<uint32_t>(packed & 0x1Fu),
                                   static_cast<uint32_t>((packed >> 5) & 0x1Fu),
                                   static_cast<uint32_t>((packed >> 10) & 0x1Fu),
                                   static_cast<uint32_t>((packed >> 15) & 0x01u)};
            std::memcpy(dst, w, 16);
        }
        break;

    case G650_UK_S_32:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            uint32_t s = 0u;
            std::memcpy(&s, src, 4);
            const uint32_t w[4] = {s, s, s, s};
            std::memcpy(dst, w, 16);
        }
        break;

    case G650_UK_S_16:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            uint16_t raw = 0u;
            std::memcpy(&raw, src, 2);
            const uint32_t s = zeroExtend
                                   ? static_cast<uint32_t>(raw)
                                   : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
            const uint32_t w[4] = {s, s, s, s};
            std::memcpy(dst, w, 16);
        }
        break;

    case G650_UK_S_8:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            const uint8_t raw = src[0];
            const uint32_t s = zeroExtend
                                   ? static_cast<uint32_t>(raw)
                                   : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
            const uint32_t w[4] = {s, s, s, s};
            std::memcpy(dst, w, 16);
        }
        break;

    case G650_UK_PART_32:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
            std::memcpy(dst, src, static_cast<size_t>(components) * 4u);
        break;

    case G650_UK_PART_16:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            for (uint32_t c = 0; c < components; ++c)
            {
                uint16_t raw = 0u;
                std::memcpy(&raw, src + c * 2u, 2);
                const uint32_t v = zeroExtend
                                       ? static_cast<uint32_t>(raw)
                                       : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                std::memcpy(dst + c * 4u, &v, 4);
            }
        }
        break;

    case G650_UK_PART_8:
        for (uint32_t i = 0; i < n; ++i, dst += 16, src += bytesPerVector)
        {
            for (uint32_t c = 0; c < components; ++c)
            {
                const uint32_t v = zeroExtend
                                       ? static_cast<uint32_t>(src[c])
                                       : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(src[c])));
                std::memcpy(dst + c * 4u, &v, 4);
            }
        }
        break;

    default:
        break;
    }
}

void g650UnpackReport()
{
    if (!g650UnpackStatOn())
        return;
    const uint64_t cmds = g_g650UnpackCmds.load(std::memory_order_relaxed);
    if (cmds == 0ull || (cmds % 200000ull) != 0ull)
        return;
    const uint64_t fast = g_g650UnpackFastCmds.load(std::memory_order_relaxed);
    const uint64_t vecs = g_g650UnpackVecs.load(std::memory_order_relaxed);
    const uint64_t fvecs = g_g650UnpackFastVecs.load(std::memory_order_relaxed);
    const uint64_t mcpy = g_g650UnpackMemcpyVecs.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G650:unpack] cmds=%llu fast=%llu (%.2f%%) vecs=%llu fastVecs=%llu (%.2f%%) "
                 "v4_32Vecs=%llu (%.2f%%) rej(mask=%llu mode=%llu cycle=%llu wrap=%llu fmt=%llu trace=%llu) "
                 "checks=%llu bad=%llu\n",
                 (unsigned long long)cmds, (unsigned long long)fast,
                 cmds ? 100.0 * (double)fast / (double)cmds : 0.0,
                 (unsigned long long)vecs, (unsigned long long)fvecs,
                 vecs ? 100.0 * (double)fvecs / (double)vecs : 0.0,
                 (unsigned long long)mcpy,
                 vecs ? 100.0 * (double)mcpy / (double)vecs : 0.0,
                 (unsigned long long)g_g650UnpackRej[0].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackRej[1].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackRej[2].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackRej[3].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackRej[4].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackRej[5].load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackChecks.load(std::memory_order_relaxed),
                 (unsigned long long)g_g650UnpackBad.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace

#endif // DC2_G650_FAST_UNPACK_INC
