// ===== G746: THE PARTIAL-DEST WRITE IS A SEVEN-INSTRUCTION BIT-SELECT ON AN SSE4.1 HOST ========
//
// ⛔ THE DEFECT. `G610Emit::storeDest` — the single site through which BOTH compiled backends write
// every VU1 FMAC destination — emits, for a dest mask that is neither `0xF` nor a single lane:
//
//     movaps xt1, [r11 + DEST + 16*dest]     ; the mask                       LOAD
//     movups xt2, [rcx + disp]               ; the OLD destination            LOAD
//     andnps xt1, xt2                        ; ~mask & old
//     movaps xt2, xv
//     andps  xt2, [r11 + DEST + 16*dest]     ; mask & value                   LOAD
//     orps   xt2, xt1
//     movups [rcx + disp], xt2                                                STORE
//
// **Seven instructions and three loads to merge four lanes.** The host is an Intel Core i7-8700
// (Coffee Lake): SSE4.1 since 2008, and `BLENDPS xmm1, xmm2, imm8` does exactly this select in ONE
// instruction and ONE uop, with the lane set as an immediate instead of a 16-byte constant-pool
// load. The whole sequence collapses to:
//
//     movups xt1, [rcx + disp]               ; the OLD destination            LOAD
//     blendps xt1, xv, imm8                  ; xt1[i] = imm8[i] ? xv[i] : xt1[i]
//     movups [rcx + disp], xt1                                                STORE
//
// ⇒ **four instructions and two loads removed per partial-dest FMAC**, and the two removed loads are
// constant-pool traffic that competes for the same load ports as the VF/ACC operands. The emitter
// was written to an SSE2 baseline because the *C++* build baseline is SSE2 (`g656_bilinear_simd.inc`
// says so in as many words) — but this emitter writes BYTES, so the compiler's ISA floor never
// applied to it. That is the "stale assumption left behind" family: a constraint on one producer
// silently inherited by a different one.
//
// ⭐ IT IS BIT-IDENTICAL, NOT MERELY EQUIVALENT. `BLENDPS` is a pure 32-bit lane select driven by an
// immediate; it performs no floating-point arithmetic, reads no MXCSR state, and raises no
// exception. `(mask & v) | (~mask & old)` lane by lane IS its definition. NaN payloads, denormals
// and signed zeros move verbatim, which is the property `DC2_G610_VERIFY` / `DC2_G612_VERIFY`
// actually test (whole `VU1State` + all eight G430 ring entries + all of VU data RAM, on raw bits).
//
// ⚠️ THE SINGLE-LANE AND FULL-DEST SHAPES ARE DELIBERATELY UNCHANGED. `dest == 0xF` is already one
// `movups`; a single lane is already `movss` (lane 0) or `movaps`+`shufps`+`movss`, which writes
// FOUR bytes. A blend would widen that to a 16-byte read-modify-write for no instruction saving.
//
// ⚠️ THE REGISTER FORM IS FIXED TOO. `g730StoreLoc`'s bound-location path spends SIX instructions on
// the same select with both operands already in registers; `blendps dr, xv, imm8` is one. G730's
// allocator ships OFF, so that hunk converts nothing today — it is here because leaving one of the
// two partial-dest selects on the slow shape is exactly how a family half-fixes itself.
//
// DETECTION. CPUID leaf 1, ECX bit 19. Checked once and cached; on a host without SSE4.1 every
// emitter falls back to the byte-identical SSE2 sequence, so the binary still runs everywhere.
//
// Levers:
//   DC2_G746_NO_BLEND=1   rollback — emit the SSE2 bit-select exactly as before
//   DC2_G746_DEST_STAT=1  [G746:dest] translate-time census of the dest-mask population
//                         (no runtime cost: bumped by the EMITTER, once per translated pair)

// ⛔⛔⛔ DEFAULT OFF — CLOSED ON CONVERSION, NOT ON CORRECTNESS. `DC2_G746_BAL=8` prices ONE emitted
// instruction at 0.0024 ns/pair, so removing the four this fix removes from 6.0 % of destination
// writes has a ceiling of ~0.0006 ns/pair against a 0.46-0.61 ns/pair route floor. Exact, smaller,
// and kept for a future emitter that is not stall-bound — see plans/phase-G746-plan.md §6.
// Arm with `DC2_G746_BLEND=1`.
#ifndef DC2_G746_BLEND_DEFAULT_ON
#define DC2_G746_BLEND_DEFAULT_ON 0
#endif

// ⚠️ `<intrin.h>` is included at the TOP of ps2_vu1.cpp, at GLOBAL scope. This file is included
// from inside vu1_g610_native_jit.inc's ANONYMOUS NAMESPACE, and pulling a system header in there
// would give every declaration it makes internal linkage.

inline bool g746HasSse41()
{
    static const bool s_has = []() -> bool {
#if defined(_MSC_VER)
        int r[4] = {0, 0, 0, 0};
        __cpuid(r, 0);
        if (r[0] < 1)
            return false;
        __cpuid(r, 1);
        return (r[2] & (1 << 19)) != 0;   // ECX.SSE4_1
#else
        return false;
#endif
    }();
    return s_has;
}

// The existing blend experiment and single-lane store experiment share CPU detection, but
// keep independent selection. Widening a store must not silently enable other destination edits.
inline bool g746BlendOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G746_NO_BLEND") &&
        (DC2_G746_BLEND_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G746_BLEND")) &&
        g746HasSse41();
    return s_on;
}

const bool s_g751WideStore = f50_12_vu_env_flag("DC2_G751_WIDE_STORE") &&
    !f50_12_vu_env_flag("DC2_G751_NO_WIDE_STORE");
const bool s_g751StoreStat = f50_12_vu_env_flag("DC2_G751_STORE_STAT");
thread_local uint64_t s_g751WideStores = 0;

// Independent translation-time experiments. No selection or counters in generated hot code.
const bool s_g752PartialAcc = f50_12_vu_env_flag("DC2_G752_PARTIAL_ACC") &&
    !f50_12_vu_env_flag("DC2_G752_NO_PARTIAL_ACC") && g746HasSse41();
const bool s_g752ClipPack = f50_12_vu_env_flag("DC2_G752_CLIP_PACK") &&
    !f50_12_vu_env_flag("DC2_G752_NO_CLIP_PACK");
const bool s_g752ScalarBc = f50_12_vu_env_flag("DC2_G752_SCALAR_BC") &&
    !f50_12_vu_env_flag("DC2_G752_NO_SCALAR_BC");
const bool s_g753PartialVf = f50_12_vu_env_flag("DC2_G753_PARTIAL_VF") &&
    !f50_12_vu_env_flag("DC2_G753_NO_PARTIAL_VF") && g746HasSse41();
const unsigned s_g753DelayPairs = [] {
    const char *v = std::getenv("DC2_G753_DELAY_PAIRS");
    const long n = v ? std::strtol(v, nullptr, 10) : 0;
    return (unsigned)(n > 0 && n <= 32 ? n : 0);
}();
const bool s_g753DelayIndependent = f50_12_vu_env_flag("DC2_G753_DELAY_INDEPENDENT");
const bool s_g754DeferredCursor = f50_12_vu_env_flag("DC2_G754_DEFER_CURSOR");
const bool s_g755ViChain = f50_12_vu_env_flag("DC2_G755_VI_CHAIN");
const bool s_g755LowerForward = f50_12_vu_env_flag("DC2_G755_LOWER_FORWARD");

inline bool g746DestStatOn()
{
    static const bool s_on = f50_12_vu_env_flag("DC2_G746_DEST_STAT");
    return s_on;
}

// ===============================================================================================
// G746 PART 2 — THE EMITTER IS TWO-OPERAND SSE ON A THREE-OPERAND HOST
//
// ⛔ THE DEFECT, AND IT IS THE SAME STALE ASSUMPTION ONE LEVEL UP. Every arithmetic pair in the
// translator reads
//
//     movaps X2, xVs        ; a copy that exists ONLY because SSE overwrites its destination
//     mulps  X2, xB
//
// and every broadcast operand reads
//
//     movups X1, [rcx + VF + 16*ft]
//     shufps X1, X1, bc*0x55
//
// VEX gives all of these a non-destructive third operand: `vmulps X2, xVs, xB` is one instruction,
// and `vpermilps X1, [rcx + VF + 16*ft], bc*0x55` is one instruction that folds the load as well.
// A MADDbc pair — the single most common VU1 form — drops from
//
//     movups movups shufps movaps mulps movups addps          (7)
// to
//     vpermilps(mem) vmulps vaddps(mem)                        (3)
//
// plus the destination write, which G746 Part 1 already cut from seven instructions to three and
// Part 2 cuts to two (`vblendps xt1, xv, [rcx+disp], imm` folds the old-value load).
//
// ⭐ EXACTNESS IS STRUCTURAL. A VEX-encoded 128-bit op computes bit-for-bit what its legacy SSE
// counterpart computes — same IEEE semantics, same MXCSR, same NaN propagation, same denormal
// handling. The ONLY architectural difference is that VEX zeroes bits 255:128 of the destination,
// which this emitter never reads. `vpermilps xd, xs, imm` is defined lane-for-lane as
// `shufps xd, xs, imm` with both shufps sources equal, which is exactly the form the translator
// emits. `vblendps` is Part 1's select with its old-value load folded.
//
// ⭐ NO AVX/SSE TRANSITION EXPOSURE. Every op emitted here is 128-bit (VEX.L = 0), and a VEX.128 op
// ZEROES the upper half of its destination. The emitted body therefore never leaves a dirty upper
// YMM for the host's legacy-SSE code to trip over — it can only ever CLEAN registers that the rest
// of the process left dirty. No `vzeroupper` is required and none is emitted.
//
// DETECTION. CPUID.1:ECX.AVX[28] and OSXSAVE[27], then XGETBV(0) & 0x6 == 0x6 so the OS is actually
// saving the YMM state. A host that fails any of those keeps the byte-identical legacy encoding.
//
// Levers:
//   DC2_G746_NO_AVX=1     rollback — emit legacy SSE exactly as before (implies the Part 1 blend
//                         still applies, since BLENDPS is SSE4.1 and independent of VEX)
//   DC2_G746_NO_FOLD=1    keep VEX encoding but not the three-operand/memory-fold call shapes, so
//                         "the prefix" and "the instruction count" can be priced apart.
// ===============================================================================================

// ⛔⛔⛔ DEFAULT OFF — same closure. The VEX re-encoding removes ~3 instructions and 3 explicit
// loads from the dominant FMAC pair, and `DC2_G746_BAL=8` prices that at ~0.0072 ns/pair = 0.022
// ms/f on `dragon`. Arm with `DC2_G746_AVX=1`.
#ifndef DC2_G746_AVX_DEFAULT_ON
#define DC2_G746_AVX_DEFAULT_ON 0
#endif

inline bool g746AvxOn()
{
    static const bool s_on = []() -> bool {
        if (f50_12_vu_env_flag("DC2_G746_NO_AVX"))
            return false;
        if (DC2_G746_AVX_DEFAULT_ON == 0 && !f50_12_vu_env_flag("DC2_G746_AVX"))
            return false;
#if defined(_MSC_VER)
        int r[4] = {0, 0, 0, 0};
        __cpuid(r, 0);
        if (r[0] < 1)
            return false;
        __cpuid(r, 1);
        const bool osxsave = (r[2] & (1 << 27)) != 0;
        const bool avx = (r[2] & (1 << 28)) != 0;
        if (!osxsave || !avx)
            return false;
        // The OS must have enabled XMM (bit 1) and YMM (bit 2) state in XCR0, or a VEX instruction
        // raises #UD however capable the silicon is.
        const unsigned long long xcr0 = _xgetbv(0);
        return (xcr0 & 0x6ull) == 0x6ull;
#else
        return false;
#endif
    }();
    return s_on;
}

// Fold the three-operand / memory-operand CALL SHAPES separately from the VEX PREFIX, so a gate can
// tell "the encoding changed" from "the instruction count changed".
inline bool g746FoldOn()
{
    static const bool s_on = g746AvxOn() && !f50_12_vu_env_flag("DC2_G746_NO_FOLD");
    return s_on;
}

// ⭐ G746 PART 3 — THE OPERAND LOAD ITSELF. A VEX arithmetic op takes its SECOND source from memory
// with no alignment requirement, so an operand that the pair reads EXACTLY ONCE, in that position,
// needs no `movups` at all: `vmulps X3, X1, [rcx + VF + 16*fs]` replaces a load plus a multiply.
//
// ⛔ WHICH operand sits in the rm position is decided by `ord.mulDstIsB` — the order G611 MEASURED
// out of the shipped interpreter, because it is observable in the NaN payload the G430 history
// stores. Folding must never move an operand across that order; it only removes the load of
// whichever operand the order already put second. Exactly one of `vs`/`b` can therefore fold, never
// both, and neither folds for MIN/MAX (the G611 subnormal-canonicalisation arm reads both a second
// time) or for CLIPw (two reads of `vs`).
inline bool g746OpFoldOn()
{
    static const bool s_on = g746FoldOn() && !f50_12_vu_env_flag("DC2_G746_NO_OPFOLD");
    return s_on;
}

// ⭐ G746 PART 4 — the G430 ring wrap. Independent of AVX and of SSE4.1: it is an 8-bit add
// replacing a 32-bit add plus a 32-bit mask, valid because `G430_HIST * sizeof(G430HistEntry)` is
// exactly 256. Rollback `DC2_G746_NO_RINGBYTE=1`.
inline bool g746RingByteOn()
{
    static const bool s_on = f50_12_vu_env_flag("DC2_G746_RINGBYTE") &&
                             !f50_12_vu_env_flag("DC2_G746_NO_RINGBYTE");
    return s_on;
}

// ⭐ G746 PART 5 — ACC residency across consecutive FMACs, with no prologue. Independent of AVX and
// of SSE4.1 (it removes a LOAD, not an encoding), so it has its own rollback:
// `DC2_G746_NO_ACCREG=1`. The argument for why it is free where G730's whole-body binding was not
// is at `G610Emit::m_accInX2`.
// ⭐⭐⭐ PROMOTED DEFAULT-ON (G746 §5.1). TWO independent order-balanced A B B A gates on `dragon`,
// same binary, env only, `pairs/call` identity 0.01 % in both:
//     ABBA 1  -0.0341 ns/pair  (drift 0.0608, inadmissible)   blocks -0.0434 / -0.0248
//     ABBA 2  -0.0348 ns/pair  (drift 0.0124, ADMISSIBLE)     blocks -0.0478 / -0.0218
// Point estimates agree to 2 %, **sign 4/4 favourable**, and `[G610:jit] codeKiB` 1092.9 -> 1080.7
// proves the arm reached the emitter in every candidate run. Exact: `[G610:verify] blocks=6,000,000
// bad=0` + `[G612:verify] regions=4,000,000 bad=0` = ~170 M pairs of whole-state comparison.
// Conversion: -0.0345 ns/pair x ~3.12 M pairs/f = **about -0.11 ms/f of VU1 busy on `dragon`**.
inline bool g746AccRegOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G746_NO_ACCREG");
    return s_on;
}

// G747: no entry/exit work. Only reuse a full vector already produced in X2.
inline bool g747VfForwardOn()
{
    static const bool s_on = f50_12_vu_env_flag("DC2_G747_VF_FORWARD") &&
                                 !f50_12_vu_env_flag("DC2_G747_NO_VF_FORWARD");
    return s_on;
}
thread_local uint64_t s_g747VfForwards = 0u;

// VEX map (`mmmmm`) and legacy-prefix (`pp`) selectors.
enum : unsigned { G746_M_0F = 1u, G746_M_0F38 = 2u, G746_M_0F3A = 3u };
enum : unsigned { G746_P_NP = 0u, G746_P_66 = 1u, G746_P_F3 = 2u, G746_P_F2 = 3u };

// The VU dest mask numbers lanes from the TOP bit down (`0x8` is lane 0 = x = the LOW 32 bits — see
// `g610BuildConsts`, `t.dest[d][lane] = (d & (0x8 >> lane))`). `BLENDPS`'s immediate numbers lanes
// from bit 0 up. The mapping is therefore a 4-bit REVERSAL, written out as a table so a wrong bit is
// a wrong constant in one place rather than a shift expression that reads correct either way.
//
//   dest 0xE (.xyz -> lanes 0,1,2) => imm8 0x07        dest 0x1 (.w -> lane 3) => imm8 0x08
constexpr uint8_t kG746BlendImm[16] = {
    0x0u, 0x8u, 0x4u, 0xCu, 0x2u, 0xAu, 0x6u, 0xEu,
    0x1u, 0x9u, 0x5u, 0xDu, 0x3u, 0xBu, 0x7u, 0xFu
};

// ⭐ G746 — THE BALLAST (Rule 88's instrument, aimed at THIS emitter). `DC2_G746_BAL=N` emits N
// extra byte-identical `movaps X5, X4` immediately after every FMAC destination write. The copies
// are pure: register-to-register, architecturally dead, on no dependency chain, and eliminated at
// rename on this host — i.e. they are EXACTLY the class of instruction Part 2 removes. Divide the
// measured `loopNsPerPair` effect by N and the answer is what ONE eliminated-move slot costs in the
// emitted stream, which is the only honest way to predict what removing ~2.3 of them per pair can
// be worth BEFORE spending a gate on it.
//
// ⚠️ IT IS NOT FREE OF CONFOUNDS: N extra instructions per pair also grow the emitted body, so a
// large N prices I-cache pressure as well as issue width. Keep N small (4 or less) and say so.
inline uint32_t g746BallastN()
{
    static const uint32_t s_n = []() -> uint32_t {
        const char *v = std::getenv("DC2_G746_BAL");
        if (v == nullptr)
            return 0u;
        const long n = std::strtol(v, nullptr, 10);
        return (n <= 0) ? 0u : (n > 32 ? 32u : (uint32_t)n);
    }();
    return s_n;
}

// ⭐⭐⭐ G746 — THE **STORE** BALLAST, and it is the one that matters after `DC2_G746_BAL` came back
// at 0.0024 ns/pair per instruction slot.
//
// `DC2_G746_BALST=N` emits N EXTRA copies of the FMAC destination store, to the SAME address with
// the SAME register — so the architectural state is bit-identical by construction (the store is
// idempotent) and `pairs/call`, the compiled population and every other invariant are unchanged.
// What it adds is N store-port slots and N store-buffer entries per FMAC pair.
//
// WHY IT IS THE RIGHT NEXT INSTRUMENT. `DC2_G746_BAL` prices an ISSUE SLOT and found eight of them
// free per pair, so the compiled pair (~31 host cycles for ~9-14 instructions) is stalled on
// something that is not issue width. A FMAC pair issues FOUR stores — three G430 history stores plus
// the destination — and a store is not a slot: it is a store-port cycle and a store-buffer entry,
// neither of which `movaps X5, X4` consumes. If a store costs an order more than a slot, then
// G744's history elision (40.8 % of pushes today) is the live lever and its remaining 59.2 % has a
// price for the first time.
//
// ⚠️ Same caveat as the slot ballast: N extra stores also grow the body. Keep N small.
inline uint32_t g746StoreBallastN()
{
    static const uint32_t s_n = []() -> uint32_t {
        const char *v = std::getenv("DC2_G746_BALST");
        if (v == nullptr)
            return 0u;
        const long n = std::strtol(v, nullptr, 10);
        return (n <= 0) ? 0u : (n > 32 ? 32u : (uint32_t)n);
    }();
    return s_n;
}

// Translate-time census. Bumped once per translated destination write, never per execution.
thread_local uint64_t s_g746DestMask[16] = {};
thread_local uint64_t s_g746DestBlendMem = 0ull;   // partial dests emitted as a memory blend
thread_local uint64_t s_g746DestBlendReg = 0ull;   // partial dests emitted as a register blend
thread_local uint64_t s_g746DestLegacy = 0ull;     // partial dests still on the SSE2 bit-select

void g746DestReport()
{
    if (!g746DestStatOn())
        return;
    uint64_t tot = 0ull, full = 0ull, lane = 0ull, part = 0ull, none = 0ull;
    for (unsigned d = 0u; d < 16u; ++d)
    {
        tot += s_g746DestMask[d];
        if (d == 0u)
            none += s_g746DestMask[d];
        else if (d == 0xFu)
            full += s_g746DestMask[d];
        else if ((d & (d - 1u)) == 0u)
            lane += s_g746DestMask[d];
        else
            part += s_g746DestMask[d];
    }
    // ⛔ `arm=` / `avx=` / `fold=` are the ARM PROOF (Rule 69/90). A gate that cannot see which
    // encoding the translator actually chose cannot tell a real null from an inert candidate, and
    // this lever has THREE independently disarmable halves.
    std::fprintf(stderr,
                 "[G746:dest] arm=%d avx=%d fold=%d opfold=%d ring8=%d accreg=%d vfwd=%d vfLoads=%llu writes=%llu | "
                 "full=%llu (%.1f%%) lane=%llu (%.1f%%) partial=%llu (%.1f%%) none=%llu | "
                 "blendMem=%llu blendReg=%llu legacy=%llu\n",
                 g746BlendOn() ? 1 : 0, g746AvxOn() ? 1 : 0, g746FoldOn() ? 1 : 0,
                 g746OpFoldOn() ? 1 : 0, g746RingByteOn() ? 1 : 0,
                 g746AccRegOn() ? 1 : 0,
                 g747VfForwardOn() ? 1 : 0, (unsigned long long)s_g747VfForwards,
                 (unsigned long long)tot,
                 (unsigned long long)full, tot ? 100.0 * (double)full / (double)tot : 0.0,
                 (unsigned long long)lane, tot ? 100.0 * (double)lane / (double)tot : 0.0,
                 (unsigned long long)part, tot ? 100.0 * (double)part / (double)tot : 0.0,
                 (unsigned long long)none,
                 (unsigned long long)s_g746DestBlendMem,
                 (unsigned long long)s_g746DestBlendReg,
                 (unsigned long long)s_g746DestLegacy);
    std::fprintf(stderr, "[G746:mask]");
    for (unsigned d = 0u; d < 16u; ++d)
        if (s_g746DestMask[d] != 0ull)
            std::fprintf(stderr, " %x=%llu", d, (unsigned long long)s_g746DestMask[d]);
    std::fprintf(stderr, "\n");
}
