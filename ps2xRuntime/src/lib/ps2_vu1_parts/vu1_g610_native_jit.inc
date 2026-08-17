// ============================================================================================
// G610 — NATIVE VU1 BLOCK COMPILATION BACKEND (x86-64)
// ============================================================================================
//
// WHAT THIS IS. The first *general* translator in this port: it turns a run of VU1 instruction
// pairs into straight-line host machine code in which every register index, every broadcast lane,
// every dest mask, every immediate and every VU-RAM displacement is a CONSTANT baked into the
// instruction stream. For the pairs it admits it deletes, by construction:
//
//   * the 16-byte `fused[]` record load per pair (a 32 KB array — the whole of this host's L1D,
//     which is why G516 v1's extra 8 B/pair measured +8.26%),
//   * the G516 upper-descriptor load and the `cls == 0` test,
//   * the four `switch`es inside `g421FastUpper` (cls, src, bc, op),
//   * the 28-way two-level indirect jump table in the lower slot (G515's annotated disassembly put
//     16.89% of the WHOLE VU1 worker on its seven instructions),
//   * every `FS()/FT()/FD()/DEST()/LIT()/LIS()/IMM11()` bit extraction and every
//     `vi[is]*16 & (dataSize-1)` address chain that is a pure function of the instruction word,
//   * the `hb->head` reload/store round trip that G504 proved is a loop-carried MEMORY dependence.
//
// It changes NO arithmetic. Every emitted SSE2 instruction is the one MSVC emits for the
// corresponding intrinsic in `g421FastUpper` / `g422FastLower`, with the same operand order (which
// is observable only in NaN propagation, and is matched deliberately). The backend is therefore
// exact by construction in the same sense as G605's span kernel — and on top of that there is a
// same-run oracle (`DC2_G610_VERIFY=1`) that re-runs every compiled block through the shipped
// generic executor from the identical pre-state and compares the ENTIRE architectural state, the
// whole G430 history ring and all of VU data RAM, word for word.
//
// WHY A JIT AND NOT MORE STATIC HANDLERS. G533/G547–G551 shipped five hand-written exact-source
// fragment handlers and they work — but G554 died on code-size inflation and G555 on the
// invocation-check tax, and between them they cover ~9% of blocks on one route. Emitted code costs
// `run()` ZERO text (the G481/G490 §5 inline cliff cannot bind text that is not in the binary),
// needs no new admission predicate (it reuses G490's, so the executed population is identical and
// the A/B compares like with like), and generalises to every microprogram the game uploads.
//
// ADMISSION. A block is compiled only if `g490RunLen` already admits it — so every G490 exclusion
// (E/I bit, G239 scalar prestall, flag consumer, G139 pair hazard, any lower that can write pc)
// still holds — AND every pair in it is in the closed op set below. Anything else leaves
// `s_g610Slots[slot].fn == nullptr` and the shipped generic block executor runs unchanged. That is
// the generic interpreter fallback, and it is the same fallback G533 relies on.
//
// ⛔ THE FIVE PRECONDITIONS. Each is CHECKED, never assumed:
//   1. `dataSize` is a power of two >= 16. Then `addr = (x*16) & (dataSize-1)` is always a multiple
//      of 16 below `dataSize`, so the interpreter's `addr + 16 <= dataSize` guard is the identity
//      and the emitted code may omit it. A block compiled for one `dataSize` is discarded if it
//      ever changes.
//   2. A compiled block is ONE G506 Q/P segment. `st.q`/`st.p` are read by `src==1/2` uppers and can
//      only be republished at a segment boundary; the wrapper re-derives the segment exactly as
//      `g490BlockBody` does and falls back when the block would split.
//   3. `g500Tail == 0`. A branch or flag-consumer tail runs inside the generic body.
//   4. The G430 ring cursor is loaded once at entry and stored once at exit — G504's own exactness
//      argument, verbatim, and sound here for the same reason: the emitted body calls NOTHING.
//   5. MIN/MAX with a partial dest needs G546's history canonicalisation; those pairs are DECLINED
//      rather than approximated.
//
// Status: default-OFF at bring-up (`DC2_G610_JIT=1`); rollback `DC2_G610_NO_JIT=1` once promoted.
// Verify: DC2_G610_VERIFY=1   Stats: DC2_G610_STAT=1   Gate: DC2_G419_AB=vujit
// ============================================================================================

#ifndef DC2_G610_JIT_BACKEND
#define DC2_G610_JIT_BACKEND 1
#endif
// PROMOTED DEFAULT-ON (G610). Rollback is `DC2_G610_NO_JIT=1`.
// Gate: `ridepod` −2.172 / −2.267 ms/f raw, −1.934 / −2.066 blocked (t = −8.21 / −7.74, sign 2/2);
// `s05` blocked −0.524 ± 0.221 (t = −2.37, 56/69 favourable). Mechanism, workload invariant held:
// `[G483:runprof] loopNsPerPair` 14.0 → 12.5 on `ridepod` (pairs/call 1379 both arms) and
// 11.0 → 10.4 on `s05` (855 both arms). Exact: `bad=0` on 256.2 M pairs of whole-state comparison
// across three microprogram sets.
#ifndef DC2_G610_DEFAULT_ON
#define DC2_G610_DEFAULT_ON 1
#endif

#if DC2_G610_JIT_BACKEND

// Win32 code-memory APIs declared directly rather than by including <windows.h>: this .inc sits in
// the middle of the VU1 translation unit and windows.h's macro surface (min/max, IN/OUT, near/far)
// would reach every later part file.
#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, unsigned long,
                                                              unsigned long);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall FlushInstructionCache(void *, const void *, size_t);
#endif

// The A/B arm accessor lives in g419_ab_instrument.inc; declared at GLOBAL scope, above this
// file's anonymous namespace, or it binds to an internal-linkage symbol and fails to link
// (appendix-dc2-project.md §3, the anonymous-namespace linkage trap).
extern int g610JitArm();
extern int g611OpsArm();
extern int g611QSplitArm();

namespace
{
    // -------------------------------------------------------------------------------------------
    // §1 ARCHITECTURAL OFFSETS, asserted against the real types so a field reorder is a BUILD
    // ERROR and never a silently wrong displacement in emitted code.
    // -------------------------------------------------------------------------------------------
    constexpr int32_t G610_OFF_VF = 0;    // float vf[32][4]
    constexpr int32_t G610_OFF_VI = 512;  // int32_t vi[16]
    constexpr int32_t G610_OFF_ACC = 576; // float acc[4]
    constexpr int32_t G610_OFF_Q = 592;
    constexpr int32_t G610_OFF_I = 600;
    constexpr int32_t G610_OFF_CLIP = 612; // G611 CLIPw: the only architectural word a compiled
                                           // upper writes outside VF/VI/ACC.

    static_assert(offsetof(VU1State, vf) == (size_t)G610_OFF_VF, "G610: vf offset");
    static_assert(offsetof(VU1State, vi) == (size_t)G610_OFF_VI, "G610: vi offset");
    static_assert(offsetof(VU1State, acc) == (size_t)G610_OFF_ACC, "G610: acc offset");
    static_assert(offsetof(VU1State, q) == (size_t)G610_OFF_Q, "G610: q offset");
    static_assert(offsetof(VU1State, i) == (size_t)G610_OFF_I, "G610: i offset");
    static_assert(offsetof(VU1State, clip) == (size_t)G610_OFF_CLIP, "G611: clip offset");
    static_assert(sizeof(float) == 4u && sizeof(int32_t) == 4u, "G610: scalar widths");

    constexpr int32_t G610_OFF_HVEC = 0;     // G430HistBlock::vec[8][4]
    constexpr int32_t G610_OFF_HSTAMP = 128; // ::stamp[8]
    constexpr int32_t G610_OFF_HDEST = 224;  // ::dest[8]
    constexpr int32_t G610_OFF_HKIND = 232;  // ::kind[8]
    constexpr int32_t G610_OFF_HHEAD = 240;  // ::head

    static_assert(offsetof(G430HistBlock, vec) == (size_t)G610_OFF_HVEC, "G610: hist vec");
    static_assert(offsetof(G430HistBlock, stamp) == (size_t)G610_OFF_HSTAMP, "G610: hist stamp");
    static_assert(offsetof(G430HistBlock, dest) == (size_t)G610_OFF_HDEST, "G610: hist dest");
    static_assert(offsetof(G430HistBlock, kind) == (size_t)G610_OFF_HKIND, "G610: hist kind");
    static_assert(offsetof(G430HistBlock, head) == (size_t)G610_OFF_HHEAD, "G610: hist head");
    static_assert(G430_HIST == 8, "G610 emits `and r10d,7` for the ring wrap");

    // -------------------------------------------------------------------------------------------
    // §2 REGISTER ASSIGNMENT. Every register used is VOLATILE in the Windows x64 ABI, so the
    // emitted function needs no prologue, no epilogue and no stack frame — its entry cost is the
    // `call` alone. This is exactly why the emitted body may never call anything.
    //
    //   RCX = VU1State*   RDX = vuData   R8 = stampBase (int64)   R9 = G430HistBlock*
    //   R10 = the G430 ring cursor       R11 = constant-pool base   RAX = integer scratch
    //   XMM0 = vs   XMM1 = operand b   XMM2 = upper result   XMM3 = memory/lower value
    //   XMM4, XMM5 = blend temporaries
    //
    // No base register's low three bits are 100, so the only SIB bytes emitted are the deliberate
    // indexed forms.
    // -------------------------------------------------------------------------------------------
    enum : unsigned
    {
        G610_RAX = 0u, G610_RCX = 1u, G610_RDX = 2u,
        G610_R8 = 8u, G610_R9 = 9u, G610_R10 = 10u, G610_R11 = 11u
    };
    enum : unsigned { G610_X0 = 0u, G610_X1 = 1u, G610_X2 = 2u, G610_X3 = 3u, G610_X4 = 4u,
                      G610_X5 = 5u };

    // -------------------------------------------------------------------------------------------
    // §3 CONSTANT TABLE. One 16-byte-aligned link-time object; the emitted prologue loads its
    // address into R11 once per block and every use is a constant displacement off it. No
    // RIP-relative fixups, no per-use 10-byte immediate.
    //
    // `dest` is CROSS-CHECKED against `g421MaskTableConst` at load time (§8) — the emitted blend
    // must use bit-for-bit the mask the interpreter's own bit-select uses.
    // -------------------------------------------------------------------------------------------
    struct G610ConstTable
    {
        alignas(16) uint32_t dest[16][4];  //   0 .. 255
        alignas(16) float itof[4][4];      // 256 .. 319 : splat 1, 1/16, 1/4096, 1/32768
        alignas(16) float ftoi[4][4];      // 320 .. 383 : splat 1, 16, 4096, 32768
        alignas(16) uint32_t absMask[4];   // 384 .. 399
        alignas(16) float vf0[4];          // 400 .. 415 : the G425 re-pin vector (0,0,0,1)
        // ---- G611 additions (CLIPw, OPMSUB/OPMULA, G546 MIN/MAX history canonicalisation) ----
        alignas(16) uint32_t zero[4];      // 416 .. 431 : pcmpeqd comparand
        alignas(16) uint32_t signMask[4];  // 432 .. 447 : 0x80000000
        alignas(16) uint32_t expMask[4];   // 448 .. 463 : 0x7F800000
        alignas(16) uint32_t wmask[4];     // 464 .. 479 : {~0,~0,~0,0} — OPMSUB's `result[3]=0.0f`
        alignas(16) uint8_t clipTab[64];   // 480 .. 543 : g488ClipTableConst, byte for byte
    };
    constexpr int32_t G610_C_DEST = 0;
    constexpr int32_t G610_C_ITOF = 256;
    constexpr int32_t G610_C_FTOI = 320;
    constexpr int32_t G610_C_ABS = 384;
    constexpr int32_t G610_C_VF0 = 400;
    constexpr int32_t G610_C_ZERO = 416;
    constexpr int32_t G610_C_SIGN = 432;
    constexpr int32_t G610_C_EXP = 448;
    constexpr int32_t G610_C_WMASK = 464;
    constexpr int32_t G610_C_CLIPTAB = 480;

    constexpr G610ConstTable g610BuildConsts()
    {
        G610ConstTable t{};
        for (unsigned d = 0u; d < 16u; ++d)
            for (unsigned lane = 0u; lane < 4u; ++lane)
                t.dest[d][lane] = (d & (0x8u >> lane)) ? 0xFFFFFFFFu : 0u;
        const float itof[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f};
        const float ftoi[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (unsigned k = 0u; k < 4u; ++k)
            for (unsigned lane = 0u; lane < 4u; ++lane)
            {
                t.itof[k][lane] = itof[k];
                t.ftoi[k][lane] = ftoi[k];
            }
        for (unsigned lane = 0u; lane < 4u; ++lane)
            t.absMask[lane] = 0x7FFFFFFFu;
        t.vf0[0] = 0.0f; t.vf0[1] = 0.0f; t.vf0[2] = 0.0f; t.vf0[3] = 1.0f;
        for (unsigned lane = 0u; lane < 4u; ++lane)
        {
            t.zero[lane] = 0u;
            t.signMask[lane] = 0x80000000u;
            t.expMask[lane] = 0x7F800000u;
            t.wmask[lane] = (lane == 3u) ? 0u : 0xFFFFFFFFu;
        }
        // G611: the SAME interleave table `g488ClipFlags` indexes, rebuilt here from the same
        // recipe. Cross-checked against `g488ClipTableConst` byte for byte at load time
        // (`g610CheckMaskTable`), exactly as the dest masks are — a hand-copied constant is the one
        // class of mistake the whole-state oracle can miss when the route never hits that index.
        for (unsigned gt = 0u; gt < 8u; ++gt)
            for (unsigned lt = 0u; lt < 8u; ++lt)
            {
                unsigned f = 0u;
                for (unsigned lane = 0u; lane < 3u; ++lane)
                {
                    if ((gt & (1u << lane)) != 0u)
                        f |= 1u << (2u * lane);
                    if ((lt & (1u << lane)) != 0u)
                        f |= 2u << (2u * lane);
                }
                t.clipTab[gt | (lt << 3u)] = (uint8_t)f;
            }
        return t;
    }
    // `constexpr`, so this is a link-time constant with no initialisation guard (G436's law).
    constexpr G610ConstTable g610Consts = g610BuildConsts();

    static_assert(offsetof(G610ConstTable, dest) == (size_t)G610_C_DEST, "G610: const dest");
    static_assert(offsetof(G610ConstTable, itof) == (size_t)G610_C_ITOF, "G610: const itof");
    static_assert(offsetof(G610ConstTable, ftoi) == (size_t)G610_C_FTOI, "G610: const ftoi");
    static_assert(offsetof(G610ConstTable, absMask) == (size_t)G610_C_ABS, "G610: const abs");
    static_assert(offsetof(G610ConstTable, vf0) == (size_t)G610_C_VF0, "G610: const vf0");
    static_assert(offsetof(G610ConstTable, zero) == (size_t)G610_C_ZERO, "G611: const zero");
    static_assert(offsetof(G610ConstTable, signMask) == (size_t)G610_C_SIGN, "G611: const sign");
    static_assert(offsetof(G610ConstTable, expMask) == (size_t)G610_C_EXP, "G611: const exp");
    static_assert(offsetof(G610ConstTable, wmask) == (size_t)G610_C_WMASK, "G611: const wmask");
    static_assert(offsetof(G610ConstTable, clipTab) == (size_t)G610_C_CLIPTAB, "G611: const clip");

    // -------------------------------------------------------------------------------------------
    // §4 CODE ARENA + SLOT TABLE
    // -------------------------------------------------------------------------------------------
    constexpr size_t G610_ARENA_BYTES = 8u * 1024u * 1024u;
    // `g490RunLen` is a uint8_t, so a block is at most 255 pairs, and the widest pair form emits
    // well under 160 bytes. 64 KB per block is a hard bound with a wide margin.
    constexpr size_t G610_MAX_BLOCK_BYTES = 64u * 1024u;

    using G610Fn = void (*)(VU1State *, uint8_t *, int64_t, G430HistBlock *);

    struct G610SlotRec
    {
        G610Fn fn = nullptr;    // nullptr = no compiled body for this block start
        uint16_t count = 0u;    // pairs this body executes; must equal the caller's `count`
        uint8_t readsQ = 0u;    // 1 = some upper in this block reads st.q (desc src == 1)
        uint8_t g611 = 0u;      // 1 = translation needed a G611 op form (the `vuops` arm gates it)
        // G611: WHICH pairs read st.q, bit j = pair j (indices >= 63 fold into bit 63). A mid-block
        // publication at index k is observable only by a reader BEFORE k, so this turns a
        // whole-block veto into a prefix test.
        uint64_t qMask = 0ull;
        // G611: the one-entry SPLIT memo. `qDelay` at a given block start is a property of where the
        // DIV sits relative to it, so it is stable per slot and a single entry hits.
        G610Fn splitFn1 = nullptr;
        G610Fn splitFn2 = nullptr;
        uint16_t splitN1 = 0u;  // 0 = memo empty
    };

    // -------------------------------------------------------------------------------------------
    // THE CODE CACHE, keyed by (block start slot, FNV-64 over the block's exact source words).
    //
    // WHY IT IS NEEDED — MEASURED, not assumed. The first working revision re-translated every
    // block on every rebuild, and the census read `compiled=577758 codeKiB=534181` over 70 s: DC2
    // re-uploads VU1 microcode about 2.6 times per FRAME (G533 already recorded three distinct
    // images on one route), so "re-emit on invalidation" means re-emitting ~285 bodies thousands of
    // times. Keying on the SOURCE WORDS instead lets the handful of live images each translate once
    // and hit from then on, whichever order they arrive in.
    //
    // EXACTNESS. The key is built from the exact 64-bit pair words the body was emitted from, read
    // out of the same precisely-invalidated `fused[]` the interpreter executes — this is G533's
    // "compare all ten source words" admission rule, generalised. A hit is not trusted on the hash
    // alone: `count` is part of the key AND is re-checked at the block entry, and the words are
    // re-hashed on every rebuild, so a stale encoding cannot survive one.
    // -------------------------------------------------------------------------------------------
    constexpr size_t G610_CACHE_SLOTS = 16384u; // power of two, open addressed, kept under 50% load
    struct G610CacheEnt
    {
        uint64_t key = 0u;   // 0 = empty
        G610Fn fn = nullptr;
        uint16_t count = 0u;
        uint8_t readsQ = 0u;
        uint8_t g611 = 0u;
        uint64_t qMask = 0ull;
    };

    struct G610Backend
    {
        uint8_t *arena = nullptr;
        size_t used = 0u;
        bool arenaFull = false;
        uint32_t dataSize = 0u;
        // 2,048 x 16 B, read ONCE PER BLOCK (~160 k/frame against ~1.6 M pairs), never per pair —
        // so this is not the per-pair L1D pressure G516 v1 was refuted on.
        std::array<G610SlotRec, PS2_VU1_CODE_SIZE / 8u> slot{};
        std::array<G610CacheEnt, G610_CACHE_SLOTS> cache{};
        size_t cacheUsed = 0u;
    };
    thread_local G610Backend s_g610;

    uint64_t g610HashBlock(uint32_t firstIndex, uint32_t count)
    {
        const G479FusedPair *const fused = s_g410Cache.fused.data();
        uint64_t h = 0xcbf29ce484222325ull;
        h = (h ^ (uint64_t)firstIndex) * 0x100000001b3ull;
        h = (h ^ (uint64_t)count) * 0x100000001b3ull;
        for (uint32_t j = 0u; j < count; ++j)
            h = (h ^ fused[firstIndex + j].source) * 0x100000001b3ull;
        return h ? h : 1ull;
    }

    // Census. All thread_local, all incremented only under DC2_G610_STAT except the two that the
    // hook needs anyway (`fn == nullptr` is a test the hook performs regardless).
    thread_local uint64_t s_g610Compiled = 0u, s_g610Refused = 0u, s_g610RefusedPairs = 0u;
    thread_local uint64_t s_g610Blocks = 0u, s_g610Pairs = 0u;
    thread_local uint64_t s_g610FbSeg = 0u, s_g610FbTail = 0u, s_g610FbNone = 0u,
                          s_g610FbCount = 0u;
    // G611 — the RANKING counters. `[G610:why]`'s translate-time totals count DISTINCT BLOCKS
    // (108/24/95), which is an events ranking; what sizes a lever is EXECUTED PAIRS. `fbSegPairs`
    // supplies the one denominator that cannot be derived from the others, so the whole remaining
    // block-eligible population splits three ways:
    //   native            = [G610:jit] nativePairs
    //   Q-refused         = fbSegPairs
    //   no compiled body  = [G490:block] pairs - nativePairs - fbSegPairs
    thread_local uint64_t s_g610FbSegPairs = 0u;
    thread_local uint64_t s_g611SplitBlocks = 0u, s_g611SplitPairs = 0u, s_g611SplitFail = 0u;
    thread_local uint64_t s_g611OpsBlocks = 0u, s_g611OpsPairs = 0u;
    thread_local uint64_t s_g610CodeBytes = 0u;
    thread_local uint64_t s_g610Verified = 0u;
    thread_local uint64_t s_g610CacheHit = 0u, s_g610CacheMiss = 0u, s_g610ArenaResets = 0u;
    thread_local uint64_t s_g610Rebuilds = 0u;
    // Why a translation was refused, indexed by G610Refuse.
    enum G610Refuse : int
    {
        G610_RF_UPPER_CLS = 0, G610_RF_UPPER_OP, G610_RF_MINMAX_DEST, G610_RF_LOWER_KIND,
        G610_RF_DATASIZE, G610_RF_ARENA, G610_RF_ENCODE, G610_RF_COUNT
    };
    thread_local uint64_t s_g610RfWhy[G610_RF_COUNT] = {};
    const char *const kG610RfName[G610_RF_COUNT] = {"upperCls", "upperOp", "minmaxDest",
                                                    "lowerKind", "dataSize", "arena", "encode"};

    bool g610StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G610_STAT");
        return s_on;
    }
    bool g610VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G610_VERIFY");
        return s_on;
    }
    // Is `DC2_G419_AB=vujit` the selected lever? Sampled once, the same way `g490BlockRequested`
    // does it, because it must be known BEFORE the arm register exists.
    bool g610Paired()
    {
        static const bool s_p = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "vujit") == 0;
        }();
        return s_p;
    }

    // PROCESS-level: does this process want compiled bodies to EXIST? True for either arm of the
    // paired A/B — otherwise a flip to the candidate arm would find an empty slot table and measure
    // nothing (G490 §2's structurally-unmeasurable-lever trap, third recurrence in this arc).
    bool g610BackendEnabled()
    {
        static const bool s_on =
            g610Paired() ||
            (!f50_12_vu_env_flag("DC2_G610_NO_JIT") &&
             (DC2_G610_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G610_JIT")));
        return s_on;
    }

    // PER-RUN() arm: does the HOOK use them. Loop-invariant, read once per run() exactly like every
    // other arm in this family (G479's rule).
    bool g610JitRun()
    {
        if (g610JitArm() >= 0)
            return g610JitArm() > 0;
        static const bool s_on =
            !f50_12_vu_env_flag("DC2_G610_NO_JIT") &&
            (DC2_G610_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G610_JIT"));
        return s_on;
    }

    // -------------------------------------------------------------------------------------------
    // §4b G611 — THE TWO COVERAGE LEVERS.
    //
    // WHY THEY ARE GATED AT THE HOOK AND NOT AT TRANSLATION. Translation happens once per code
    // upload in a cold pass; a per-run() A/B arm cannot reach it. So both levers emit their bodies
    // UNCONDITIONALLY whenever the lever is selected, tag the affected blocks, and let the hook
    // decide per run() whether to use the tag. That keeps the two arms comparing the same
    // population with the same slot table (G490 §2), at the price of one flag test per BLOCK entry
    // — never per pair.
    //
    // The `*Emit` accessors are PROCESS level (do the bodies exist at all); the `*Run` accessors are
    // the per-run() arm. Rollbacks `DC2_G611_NO_OPS` / `DC2_G611_NO_QSPLIT` act at translation, so
    // a rolled-back binary carries no runtime cost for the lever it disables.
    // -------------------------------------------------------------------------------------------
    bool g611OpsPaired()
    {
        static const bool s_p = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "vuops") == 0;
        }();
        return s_p;
    }
    bool g611QSplitPaired()
    {
        static const bool s_p = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "vuqsplit") == 0;
        }();
        return s_p;
    }
    // Should the translator EMIT the G611 op forms (CLIPw, OPMSUB/OPMULA, MIN/MAX partial dest)?
    bool g611OpsEmit()
    {
        static const bool s_on = g611OpsPaired() || !f50_12_vu_env_flag("DC2_G611_NO_OPS");
        return s_on;
    }
    // Should the translator emit the SUB-BLOCK bodies the Q/P split needs?
    bool g611QSplitEmit()
    {
        static const bool s_on = g611QSplitPaired() || !f50_12_vu_env_flag("DC2_G611_NO_QSPLIT");
        return s_on;
    }
    bool g611OpsRun()
    {
        if (g611OpsArm() >= 0)
            return g611OpsArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G611_NO_OPS");
        return s_on;
    }
    bool g611QSplitRun()
    {
        if (g611QSplitArm() >= 0)
            return g611QSplitArm() > 0;
        static const bool s_on = !f50_12_vu_env_flag("DC2_G611_NO_QSPLIT");
        return s_on;
    }

    // -------------------------------------------------------------------------------------------
    // §5 THE ENCODER. Deliberately tiny and total: one helper per instruction form, nothing
    // chooses an encoding at run time, so a wrong byte is a wrong CONSTANT in exactly one place.
    // -------------------------------------------------------------------------------------------
    struct G610Emit
    {
        uint8_t *p = nullptr;
        size_t n = 0u;
        size_t cap = 0u;
        bool bad = false;

        void u8(uint8_t v)
        {
            if (n >= cap) { bad = true; return; }
            p[n++] = v;
        }
        void u32(uint32_t v)
        {
            u8((uint8_t)v); u8((uint8_t)(v >> 8)); u8((uint8_t)(v >> 16)); u8((uint8_t)(v >> 24));
        }
        void i32(int32_t v) { u32((uint32_t)v); }
        void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }

        // REX, emitted only when a field needs it.
        void rex(bool w, unsigned reg, unsigned index, unsigned base)
        {
            const uint8_t b = (uint8_t)(0x40u | (w ? 8u : 0u) | ((reg >= 8u) ? 4u : 0u) |
                                        ((index >= 8u) ? 2u : 0u) | ((base >= 8u) ? 1u : 0u));
            if (b != 0x40u)
                u8(b);
        }
        void memD(unsigned reg, unsigned base, int32_t disp)   // [base + disp32]
        {
            u8((uint8_t)(0x80u | ((reg & 7u) << 3) | (base & 7u)));
            i32(disp);
        }
        void memX(unsigned reg, unsigned base, unsigned index, unsigned scale, int32_t disp)
        {                                                      // [base + index*scale + disp32]
            const unsigned s = (scale == 1u) ? 0u : (scale == 2u) ? 1u : (scale == 4u) ? 2u : 3u;
            u8((uint8_t)(0x80u | ((reg & 7u) << 3) | 4u));
            u8((uint8_t)((s << 6) | ((index & 7u) << 3) | (base & 7u)));
            i32(disp);
        }
        void modRR(unsigned reg, unsigned rm)
        { u8((uint8_t)(0xC0u | ((reg & 7u) << 3) | (rm & 7u))); }

        // ---- SSE (0F-mapped) ----
        void sseM(uint8_t op, unsigned xr, unsigned base, int32_t d)
        { rex(false, xr, 0u, base); u8(0x0F); u8(op); memD(xr, base, d); }
        void sseMX(uint8_t op, unsigned xr, unsigned base, unsigned ix, unsigned sc, int32_t d)
        { rex(false, xr, ix, base); u8(0x0F); u8(op); memX(xr, base, ix, sc, d); }
        void sseR(uint8_t op, unsigned xd, unsigned xs)
        { rex(false, xd, 0u, xs); u8(0x0F); u8(op); modRR(xd, xs); }
        void sseF3M(uint8_t op, unsigned xr, unsigned base, int32_t d)
        { u8(0xF3); rex(false, xr, 0u, base); u8(0x0F); u8(op); memD(xr, base, d); }
        void sseF3MX(uint8_t op, unsigned xr, unsigned base, unsigned ix, int32_t d)
        { u8(0xF3); rex(false, xr, ix, base); u8(0x0F); u8(op); memX(xr, base, ix, 1u, d); }
        void sseF3R(uint8_t op, unsigned xd, unsigned xs)
        { u8(0xF3); rex(false, xd, 0u, xs); u8(0x0F); u8(op); modRR(xd, xs); }
        void sse66R(uint8_t op, unsigned xd, unsigned rs)
        { u8(0x66); rex(false, xd, 0u, rs); u8(0x0F); u8(op); modRR(xd, rs); }

        // [index*scale + disp32], no base — ModRM mod=00 rm=100 with SIB base=101.
        void memIdxNoBase(unsigned reg, unsigned index, unsigned scale, int32_t disp)
        {
            const unsigned s = (scale == 1u) ? 0u : (scale == 2u) ? 1u : (scale == 4u) ? 2u : 3u;
            u8((uint8_t)(0x00u | ((reg & 7u) << 3) | 4u));
            u8((uint8_t)((s << 6) | ((index & 7u) << 3) | 5u));
            i32(disp);
        }
        void lea64Idx(unsigned rd, unsigned index, unsigned scale, int32_t disp)
        { rex(true, rd, index, 0u); u8(0x8D); memIdxNoBase(rd, index, scale, disp); }

        void movupsLd(unsigned x, unsigned b, int32_t d) { sseM(0x10, x, b, d); }
        void movupsStXS(unsigned x, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { sseMX(0x11, x, b, ix, sc, d); }
        void movupsSt(unsigned x, unsigned b, int32_t d) { sseM(0x11, x, b, d); }
        void movupsLdX(unsigned x, unsigned b, unsigned ix, int32_t d) { sseMX(0x10, x, b, ix, 1u, d); }
        void movupsStX(unsigned x, unsigned b, unsigned ix, int32_t d) { sseMX(0x11, x, b, ix, 1u, d); }
        void movapsLd(unsigned x, unsigned b, int32_t d) { sseM(0x28, x, b, d); }
        void movapsStX(unsigned x, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { sseMX(0x29, x, b, ix, sc, d); }
        void movaps(unsigned xd, unsigned xs) { sseR(0x28, xd, xs); }
        void movssLd(unsigned x, unsigned b, int32_t d) { sseF3M(0x10, x, b, d); }
        void movssSt(unsigned x, unsigned b, int32_t d) { sseF3M(0x11, x, b, d); }
        void movssLdX(unsigned x, unsigned b, unsigned ix, int32_t d) { sseF3MX(0x10, x, b, ix, d); }
        void movssStX(unsigned x, unsigned b, unsigned ix, int32_t d) { sseF3MX(0x11, x, b, ix, d); }
        void movdToX(unsigned x, unsigned r) { sse66R(0x6E, x, r); }
        void shufps(unsigned xd, unsigned xs, uint8_t imm) { sseR(0xC6, xd, xs); u8(imm); }
        void addps(unsigned xd, unsigned xs) { sseR(0x58, xd, xs); }
        void mulps(unsigned xd, unsigned xs) { sseR(0x59, xd, xs); }
        void subps(unsigned xd, unsigned xs) { sseR(0x5C, xd, xs); }
        void minps(unsigned xd, unsigned xs) { sseR(0x5D, xd, xs); }
        void maxps(unsigned xd, unsigned xs) { sseR(0x5F, xd, xs); }
        void andps(unsigned xd, unsigned xs) { sseR(0x54, xd, xs); }
        void andnps(unsigned xd, unsigned xs) { sseR(0x55, xd, xs); }
        void orps(unsigned xd, unsigned xs) { sseR(0x56, xd, xs); }
        void mulpsM(unsigned x, unsigned b, int32_t d) { sseM(0x59, x, b, d); }
        void addpsM(unsigned x, unsigned b, int32_t d) { sseM(0x58, x, b, d); }
        void andpsM(unsigned x, unsigned b, int32_t d) { sseM(0x54, x, b, d); }
        void cvtdq2ps(unsigned xd, unsigned xs) { sseR(0x5B, xd, xs); }
        void cvttps2dq(unsigned xd, unsigned xs) { sseF3R(0x5B, xd, xs); }

        // ---- G611 SSE additions ----
        void xorps(unsigned xd, unsigned xs) { sseR(0x57, xd, xs); }
        void xorpsM(unsigned x, unsigned b, int32_t d) { sseM(0x57, x, b, d); }
        void orpsM(unsigned x, unsigned b, int32_t d) { sseM(0x56, x, b, d); }
        void unpckhps(unsigned xd, unsigned xs) { sseR(0x15, xd, xs); }
        // cmpps xd, xs, imm8. Predicate 1 = LT, ordered and signalling — the encoding MSVC emits
        // for `_mm_cmplt_ps`, and for `_mm_cmpgt_ps` with the operands exchanged.
        void cmpltps(unsigned xd, unsigned xs) { sseR(0xC2, xd, xs); u8(0x01); }
        // pcmpeqd xd, [base+disp]  (66 0F 76 /r)
        void pcmpeqdM(unsigned x, unsigned b, int32_t d)
        { u8(0x66); rex(false, x, 0u, b); u8(0x0F); u8(0x76); memD(x, b, d); }
        void porR(unsigned xd, unsigned xs)
        { u8(0x66); rex(false, xd, 0u, xs); u8(0x0F); u8(0xEB); modRR(xd, xs); }
        // movmskps r32, xmm  (0F 50 /r) — reg field is the GPR, rm is the XMM.
        void movmskps(unsigned r, unsigned x)
        { rex(false, r, 0u, x); u8(0x0F); u8(0x50); modRR(r, x); }
        void movdFromX(unsigned r, unsigned x) { u8(0x66); rex(false, x, 0u, r); u8(0x0F); u8(0x7E);
                                                 modRR(x, r); }

        // ---- G611 integer additions ----
        // movzx r32, BYTE [base + index + disp32]
        void movzx8MX(unsigned r, unsigned b, unsigned ix, int32_t d)
        { rex(false, r, ix, b); u8(0x0F); u8(0xB6); memX(r, b, ix, 1u, d); }
        void shlMemImm(unsigned b, int32_t d, uint8_t v)   // shl DWORD [b+d], imm8
        { rex(false, 0u, 0u, b); u8(0xC1); memD(4u, b, d); u8(v); }
        void orMemReg(unsigned r, unsigned b, int32_t d)   // or DWORD [b+d], r32
        { rex(false, r, 0u, b); u8(0x09); memD(r, b, d); }

        // ---- integer ----
        void movLd32(unsigned r, unsigned b, int32_t d)
        { rex(false, r, 0u, b); u8(0x8B); memD(r, b, d); }
        void movSt32(unsigned r, unsigned b, int32_t d)
        { rex(false, r, 0u, b); u8(0x89); memD(r, b, d); }
        void movzx16(unsigned r, unsigned b, int32_t d)
        { rex(false, r, 0u, b); u8(0x0F); u8(0xB7); memD(r, b, d); }
        void movsx16M(unsigned r, unsigned b, int32_t d)
        { rex(false, r, 0u, b); u8(0x0F); u8(0xBF); memD(r, b, d); }
        void movsx16R(unsigned rd, unsigned rs)
        { rex(false, rd, 0u, rs); u8(0x0F); u8(0xBF); modRR(rd, rs); }
        // movsx r32, WORD [base + index + disp32]. ⭐ This one instruction IS the interpreter's
        // `(int32_t)(int16_t)(value & 0xFFFF)` on a 32-bit VU-RAM word, and because x86 forms the
        // address before writing the destination it may reuse the index register as the destination
        // — which is what lets ILW/ILWR be emitted without a second integer scratch.
        void movsx16MX(unsigned r, unsigned b, unsigned ix, int32_t d)
        { rex(false, r, ix, b); u8(0x0F); u8(0xBF); memX(r, b, ix, 1u, d); }
        void addImm(unsigned r, int32_t v) { rex(false, 0u, 0u, r); u8(0x81); modRR(0u, r); i32(v); }
        void andImm(unsigned r, int32_t v) { rex(false, 0u, 0u, r); u8(0x81); modRR(4u, r); i32(v); }
        void addM(unsigned r, unsigned b, int32_t d)   // add r32, [b+d]
        { rex(false, r, 0u, b); u8(0x03); memD(r, b, d); }
        void subM(unsigned r, unsigned b, int32_t d)   // sub r32, [b+d]
        { rex(false, r, 0u, b); u8(0x2B); memD(r, b, d); }
        void andM(unsigned r, unsigned b, int32_t d)   // and r32, [b+d]
        { rex(false, r, 0u, b); u8(0x23); memD(r, b, d); }
        void orM(unsigned r, unsigned b, int32_t d)    // or  r32, [b+d]
        { rex(false, r, 0u, b); u8(0x0B); memD(r, b, d); }
        void shlImm(unsigned r, uint8_t v) { rex(false, 0u, 0u, r); u8(0xC1); modRR(4u, r); u8(v); }
        void movImm32(unsigned r, uint32_t v)
        { rex(false, 0u, 0u, r); u8((uint8_t)(0xB8u + (r & 7u))); u32(v); }
        void movImm64(unsigned r, uint64_t v)
        { rex(true, 0u, 0u, r); u8((uint8_t)(0xB8u + (r & 7u))); u64(v); }
        void movStImm32(unsigned b, int32_t d, uint32_t v)
        { rex(false, 0u, 0u, b); u8(0xC7); memD(0u, b, d); u32(v); }
        void movStImm8X(unsigned b, unsigned ix, int32_t d, uint8_t v)
        { rex(false, 0u, ix, b); u8(0xC6); memX(0u, b, ix, 1u, d); u8(v); }
        void movSt64X(unsigned r, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { rex(true, r, ix, b); u8(0x89); memX(r, b, ix, sc, d); }
        void movStX32(unsigned r, unsigned b, unsigned ix, int32_t d)
        { rex(false, r, ix, b); u8(0x89); memX(r, b, ix, 1u, d); }
        void lea64(unsigned rd, unsigned b, int32_t d)
        { rex(true, rd, 0u, b); u8(0x8D); memD(rd, b, d); }
        void incR32(unsigned r) { rex(false, 0u, 0u, r); u8(0xFF); modRR(0u, r); }
        void ret() { u8(0xC3); }

        // ---- composites the translator uses more than once ----

        // Splat lane `l` of xs into all four lanes of xd.
        void splat(unsigned xd, unsigned xs, unsigned l)
        {
            if (xd != xs)
                movaps(xd, xs);
            shufps(xd, xd, (uint8_t)(l * 0x55u));
        }

        // `g428StoreDest` / `g422ApplyDest`, dest-shape specialised exactly as G428 ships it.
        // value is in `xv` (preserved); xt1/xt2 are scratch. base+disp is the destination vector.
        void storeDest(unsigned xv, unsigned base, int32_t disp, uint8_t dest,
                       unsigned xt1, unsigned xt2)
        {
            if (dest == 0u)
                return;                              // applyDest writes nothing
            if (dest == 0xFu)
            {
                movupsSt(xv, base, disp);             // all four lanes assigned
                return;
            }
            // exactly one lane?
            if ((dest & (uint8_t)(dest - 1u)) == 0u)
            {
                const unsigned lane = (dest == 0x8u) ? 0u : (dest == 0x4u) ? 1u
                                    : (dest == 0x2u) ? 2u : 3u;
                if (lane == 0u)
                {
                    movssSt(xv, base, disp);
                }
                else
                {
                    movaps(xt1, xv);
                    shufps(xt1, xt1, (uint8_t)(lane * 0x55u));
                    movssSt(xt1, base, disp + (int32_t)(4u * lane));
                }
                return;
            }
            // genuinely partial: the same bit-select the interpreter performs.
            //   xt1 = mask ; xt2 = old ; xt1 = ~mask & old ; xt2 = mask & value ; store xt1|xt2
            movapsLd(xt1, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
            movupsLd(xt2, base, disp);
            andnps(xt1, xt2);                        // xt1 = ~xt1 & xt2
            movaps(xt2, xv);
            andpsM(xt2, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
            orps(xt2, xt1);
            movupsSt(xt2, base, disp);
        }

        // `st.vi[dst] = (int16_t)value_in_eax` (sign-extended back into the int32 slot), or nothing
        // when dst == 0 — the VU0 register is architecturally read-only and the interpreter's
        // `if (x != 0)` guards say exactly this.
        void storeViTrunc(unsigned dst)
        {
            movsx16R(G610_RAX, G610_RAX);
            movSt32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * dst));
        }
        void storeViRaw(unsigned dst)
        { movSt32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * dst)); }
    };

    // -------------------------------------------------------------------------------------------
    // §6 THE TRANSLATOR
    // -------------------------------------------------------------------------------------------

    // `addr = ((uint32_t)(uint16_t)st.vi[r]) * 16u & (dataSize - 1)` into EAX.
    void g610EmitAddrU16(G610Emit &e, unsigned r, uint32_t dataSize)
    {
        e.movzx16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * r));
        e.shlImm(G610_RAX, 4u);
        e.andImm(G610_RAX, (int32_t)(dataSize - 1u));
    }
    // `addr = ((uint32_t)(int32_t)(st.vi[r] + imm)) * 16u & (dataSize - 1)` into EAX.
    void g610EmitAddrImm(G610Emit &e, unsigned r, int32_t imm, uint32_t dataSize)
    {
        e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * r));
        if (imm != 0)
            e.addImm(G610_RAX, imm);
        e.shlImm(G610_RAX, 4u);
        e.andImm(G610_RAX, (int32_t)(dataSize - 1u));
    }
    // `if (r != 0) st.vi[r] = (int16_t)(st.vi[r] + delta)` — the LQI/SQI/LQD/SQD side effect.
    void g610EmitViStep(G610Emit &e, unsigned r, int32_t delta)
    {
        if (r == 0u)
            return;
        e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * r));
        e.addImm(G610_RAX, delta);
        e.storeViTrunc(r);
    }

    // Blend `xv` into the 16 bytes of VU RAM at [rdx + eax], lane-selected by `dest`, exactly as
    // SQ/SQI/SQD do (read-modify-write of the quadword through a float temporary).
    void g610EmitStoreQuadMasked(G610Emit &e, unsigned xv, uint8_t dest)
    {
        if (dest == 0u)
            return;
        if (dest == 0xFu)
        {
            e.movupsStX(xv, G610_RDX, G610_RAX, 0);
            return;
        }
        if ((dest & (uint8_t)(dest - 1u)) == 0u)
        {
            const unsigned lane = (dest == 0x8u) ? 0u : (dest == 0x4u) ? 1u
                                : (dest == 0x2u) ? 2u : 3u;
            if (lane == 0u)
            {
                e.movssStX(xv, G610_RDX, G610_RAX, 0);
            }
            else
            {
                e.movaps(G610_X4, xv);
                e.shufps(G610_X4, G610_X4, (uint8_t)(lane * 0x55u));
                e.movssStX(G610_X4, G610_RDX, G610_RAX, (int32_t)(4u * lane));
            }
            return;
        }
        e.movapsLd(G610_X4, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
        e.movupsLdX(G610_X5, G610_RDX, G610_RAX, 0);
        e.andnps(G610_X4, G610_X5);                  // ~mask & old
        e.movaps(G610_X5, xv);
        e.andpsM(G610_X5, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
        e.orps(G610_X5, G610_X4);
        e.movupsStX(G610_X5, G610_RDX, G610_RAX, 0);
    }

    // -------------------------------------------------------------------------------------------
    // §5b ⭐⭐⭐ G611 — THE OPERAND ORDER IS **MEASURED**, NOT ASSERTED.
    //
    // G610's header says "every emitted SSE2 instruction is the one MSVC emits for the corresponding
    // intrinsic, IN THE SAME OPERAND ORDER — observable only for NaN inputs, and matched
    // deliberately." The first half is a claim about MSVC's codegen and **it is not true for the
    // commutative forms**. The G611 oracle caught it on a plain ADD:
    //
    //     hist.vec[1] jit=... fffffffc ...   gen=... fffffffa ...
    //     j=8 upK=0x28 (ADD) fs=24 ft=23     pre vf[24].z=fffffffc  vf[23].z=fffffffa
    //
    // Both operands were NaN, `ADDPS` returns the DESTINATION operand's NaN, and the shipped
    // interpreter returned **`b`'s** — i.e. MSVC compiled `_mm_add_ps(vs, b)` as `addps b, vs`.
    // C++ gives the compiler that freedom (IEEE addition is commutative for every non-NaN input and
    // the standard says nothing about payload propagation), so the order cannot be read off the
    // source at all. It is invisible until two NaNs meet, which is why 256 M verified pairs never
    // saw it: NaN garbage lives in the masked-off lanes of specific register neighbourhoods.
    //
    // ⛔ SO DO NOT READ THE ORDER OUT OF THE SOURCE, AND DO NOT GUESS IT PER OP. Run the SHIPPED
    // `g421FastUpper` once per upper KIND at start-up with distinguishable quiet-NaN payloads in
    // `vs`, `b` and `ACC`, read which payload comes back out of the G430 history entry, and emit to
    // match. The non-commutative forms (SUB, MSUB's outer subtract, MIN/MAX — `MINPS`/`MAXPS`
    // return the SOURCE operand when either input is NaN and therefore cannot be commuted at all)
    // are ASSERTED rather than followed: if one of them ever probes the other way the compiler has
    // changed the arithmetic, not the payload, and that must abort rather than be mirrored.
    // -------------------------------------------------------------------------------------------
    struct G611OpOrder
    {
        uint8_t probed = 0u;
        uint8_t mulDstIsB = 0u;   // the (vs x b) product: 1 = MSVC made `b` the destination
        uint8_t outerDstIsMul = 0u; // MADD's `acc + product`: 1 = the PRODUCT is the destination
    };
    thread_local G611OpOrder s_g611Ord[0x100];

    constexpr uint32_t G611_NAN_VS = 0x7FC0000Au;
    constexpr uint32_t G611_NAN_B = 0x7FC0000Bu;
    constexpr uint32_t G611_NAN_ACC = 0x7FC0000Cu;
    constexpr uint32_t G611_ONE = 0x3F800000u;

    // Run the shipped fast upper ONCE for `kind` with fs=1, ft=2, fd=3, dest=0xF and the given raw
    // bit patterns, and return lane 0 of the G430 history entry it pushed. The template arguments
    // are the block leaf's own `<G501Pin, G504Cursor, G516Desc>`.
    uint32_t g611RunShippedUpper(uint8_t kind, uint32_t vsBits, uint32_t bBits, uint32_t accBits)
    {
        VU1State st{};
        G430HistBlock hb{};
        for (unsigned l = 0u; l < 4u; ++l)
        {
            ((uint32_t *)st.vf[1])[l] = vsBits;
            ((uint32_t *)st.vf[2])[l] = bBits;
            ((uint32_t *)st.acc)[l] = accBits;
        }
        std::memcpy(&st.q, &bBits, 4);   // src == 1 reads Q
        std::memcpy(&st.i, &bBits, 4);   // src == 2 reads I
        const uint32_t upper = (0xFu << 21) | (2u << 16) | (1u << 11) | (3u << 6);
        uint32_t descBits = 0u;
        std::memcpy(&descBits, &g421DescTableConst.e[kind], sizeof(descBits));
        int slot = 0;
        hb.head = 0;
        (void)g421FastUpper<true, true, true>(st, upper, kind, false, false, true, 0, &hb, slot,
                                              descBits);
        return ((const uint32_t *)hb.vec[0])[0];
    }

    void g611OrderFail(uint8_t kind, const char *what, uint32_t got)
    {
        std::fprintf(stderr,
                     "[G611:order-BAD] kind=0x%02x %s got=%08x — the shipped interpreter's "
                     "arithmetic changed, not just its NaN payload. Refusing to mirror it.\n",
                     (unsigned)kind, what, got);
        std::fflush(stderr);
        std::abort();
    }

    void g611ProbeOrder()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        for (unsigned k = 0u; k < 0x100u; ++k)
        {
            const G421UpperDesc d = g421DescTableConst.e[k];
            s_g611Ord[k].probed = 1u;
            if (d.cls != 1u && d.cls != 2u)
                continue;                       // no binary FP op: nothing to order
            const uint8_t kind = (uint8_t)k;
            switch (d.op)
            {
            case 0u: // ADD — commutative, follow the compiler
            case 4u: // MUL — commutative, follow the compiler
            {
                const uint32_t r = g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, 0u);
                if (r == G611_NAN_B) s_g611Ord[k].mulDstIsB = 1u;
                else if (r != G611_NAN_VS) g611OrderFail(kind, "add/mul", r);
                break;
            }
            case 1u: // SUB — NOT commutable: SUBPS's destination is the minuend
            {
                const uint32_t r = g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, 0u);
                if (r != G611_NAN_VS) g611OrderFail(kind, "sub", r);
                break;
            }
            case 5u: // MAX
            case 6u: // MIN — MAXPS/MINPS return the SOURCE operand when either input is NaN,
            {       //       so a swap would change the VALUE and cannot happen.
                const uint32_t r = g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, 0u);
                if (r != G611_NAN_B) g611OrderFail(kind, "min/max", r);
                break;
            }
            case 2u: // MADD : acc + (vs * b) — BOTH operations are commutative
            {
                const uint32_t inner =
                    g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_ONE);
                if (inner == G611_NAN_B) s_g611Ord[k].mulDstIsB = 1u;
                else if (inner != G611_NAN_VS) g611OrderFail(kind, "madd.mul", inner);
                const uint32_t outer =
                    g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_NAN_ACC);
                if (outer == inner) s_g611Ord[k].outerDstIsMul = 1u;
                else if (outer != G611_NAN_ACC) g611OrderFail(kind, "madd.add", outer);
                break;
            }
            case 3u: // MSUB : acc - (vs * b) — the subtract is NOT commutable
            {
                const uint32_t inner =
                    g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_ONE);
                if (inner == G611_NAN_B) s_g611Ord[k].mulDstIsB = 1u;
                else if (inner != G611_NAN_VS) g611OrderFail(kind, "msub.mul", inner);
                const uint32_t outer =
                    g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_NAN_ACC);
                if (outer != G611_NAN_ACC) g611OrderFail(kind, "msub.sub", outer);
                break;
            }
            case 7u: // OPMSUB : acc - (a x bb), lanes 0..2
            case 8u: // OPMULA : (a x bb) with lane 3 from ACC
            {
                const uint32_t inner =
                    g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_ONE);
                if (inner == G611_NAN_B) s_g611Ord[k].mulDstIsB = 1u;
                else if (inner != G611_NAN_VS) g611OrderFail(kind, "op.mul", inner);
                if (d.op == 7u)
                {
                    const uint32_t outer =
                        g611RunShippedUpper(kind, G611_NAN_VS, G611_NAN_B, G611_NAN_ACC);
                    if (outer != G611_NAN_ACC) g611OrderFail(kind, "opmsub.sub", outer);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    // G611 — `g546FlushCompareSubnormals`, emitted. Result lands in `t1`; `t1` and `t2` are
    // clobbered and `v` is preserved. Two temporaries is the minimum, which is what lets both
    // operands be flushed with only X3/X4/X5 free (X0 = vs, X1 = b and X2 = the CPU result all stay
    // live for the select and the blend).
    //
    // `andps`/`andnps`/`orps` stand in for the source's `_mm_and_si128`/`_mm_andnot_si128`/
    // `_mm_or_si128`: same bits, different execution domain, and this comparison is on raw bits.
    void g611EmitFlushSubnormals(G610Emit &e, unsigned v, unsigned t1, unsigned t2)
    {
        e.movaps(t1, v);
        e.andpsM(t1, G610_R11, G610_C_ABS);            // absBits
        e.movaps(t2, t1);
        e.andpsM(t2, G610_R11, G610_C_EXP);            // expBits
        e.pcmpeqdM(t2, G610_R11, G610_C_ZERO);         // expZero
        e.pcmpeqdM(t1, G610_R11, G610_C_ZERO);         // valueZero
        e.andnps(t1, t2);                              // subnormal = ~valueZero & expZero
        e.movaps(t2, v);
        e.andpsM(t2, G610_R11, G610_C_SIGN);           // signedZero
        e.andps(t2, t1);                               // subnormal & signedZero
        e.andnps(t1, v);                               // ~subnormal & bits
        e.orps(t1, t2);
    }

    // Emit the UPPER slot. Returns false to refuse the whole block.
    bool g610EmitUpper(G610Emit &e, uint32_t upper, uint8_t kind, uint32_t pairIndexInBlock,
                       bool &usesHist, bool &usedG611, int *why)
    {
        if (kind == 0x6fu)
            return true;                              // VNOP
        const G421UpperDesc d = g421DescTableConst.e[kind];
        if (d.cls == 0u) { *why = G610_RF_UPPER_CLS; return false; }
        const bool g611Ops = g611OpsEmit();
        if (d.cls == 8u && !g611Ops) { *why = G610_RF_UPPER_CLS; return false; }
        if ((d.op == 7u || d.op == 8u) && !g611Ops) { *why = G610_RF_UPPER_OP; return false; }

        const unsigned fs = FS(upper), ft = FT(upper), fd = FD(upper);
        const uint8_t dest = DEST(upper);

        e.movupsLd(G610_X0, G610_RCX, G610_OFF_VF + (int32_t)(16u * fs));

        if (d.cls == 8u)
        {
            // G611 CLIPw — `st.clip = (st.clip << 6) | g488ClipFlags(vs, vt)`. The only compiled
            // upper that writes neither VF nor ACC, pushes no G430 history and touches no MAC.
            //
            // The two 3-bit movemasks have to be interleaved and SSE2 has no PDEP, so `g488ClipFlags`
            // uses a 64-byte table indexed by `gt | (lt << 3)`. That table is in the constant pool,
            // rebuilt from the same recipe and cross-checked byte for byte against
            // `g488ClipTableConst` at load time.
            //
            // ⭐ ONLY ONE INTEGER SCRATCH EXISTS (RAX; R11 holds the pool base and every other
            // register is an argument or the ring cursor), so the two movemask results are joined in
            // an XMM instead of a second GPR: park `lt << 3`, compute `gt`, `por`, read back.
            usedG611 = true;
            e.movupsLd(G610_X1, G610_RCX, G610_OFF_VF + (int32_t)(16u * ft));
            e.movaps(G610_X2, G610_X1);
            e.shufps(G610_X2, G610_X2, 0xFFu);          // vt.w splat
            e.andpsM(G610_X2, G610_R11, G610_C_ABS);    // aw = |vt.w|
            e.movaps(G610_X3, G610_X2);
            e.xorpsM(G610_X3, G610_R11, G610_C_SIGN);   // nw = -|vt.w|
            e.movaps(G610_X4, G610_X2);
            e.cmpltps(G610_X4, G610_X0);                // aw < vs   == vs > aw  (ordered)
            e.movaps(G610_X5, G610_X0);
            e.cmpltps(G610_X5, G610_X3);                // vs < nw               (ordered)
            e.movmskps(G610_RAX, G610_X5);
            e.andImm(G610_RAX, 7);
            e.shlImm(G610_RAX, 3u);
            e.movdToX(G610_X2, G610_RAX);               // park lt << 3
            e.movmskps(G610_RAX, G610_X4);
            e.andImm(G610_RAX, 7);
            e.movdToX(G610_X3, G610_RAX);
            e.porR(G610_X2, G610_X3);
            e.movdFromX(G610_RAX, G610_X2);             // index = gt | (lt << 3)
            e.movzx8MX(G610_RAX, G610_R11, G610_RAX, G610_C_CLIPTAB);
            e.shlMemImm(G610_RCX, G610_OFF_CLIP, 6u);
            e.orMemReg(G610_RAX, G610_RCX, G610_OFF_CLIP);
            return true;
        }

        if (d.cls >= 3u)
        {
            switch (d.cls)
            {
            case 3u: // ITOF : (float)bits(vs) * scale     -> VF[ft]
                e.cvtdq2ps(G610_X2, G610_X0);
                e.mulpsM(G610_X2, G610_R11, G610_C_ITOF + (int32_t)(16u * d.bc));
                break;
            case 4u: // FTOI : bits((int32_t)(vs * scale))  -> VF[ft]
                e.movaps(G610_X2, G610_X0);
                e.mulpsM(G610_X2, G610_R11, G610_C_FTOI + (int32_t)(16u * d.bc));
                e.cvttps2dq(G610_X2, G610_X2);
                break;
            case 5u: // ABS
                e.movaps(G610_X2, G610_X0);
                e.andpsM(G610_X2, G610_R11, G610_C_ABS);
                break;
            case 6u: // MOVE
                e.movaps(G610_X2, G610_X0);
                break;
            case 7u: // MR32 : {vs.y, vs.z, vs.w, vs.x}
                e.movaps(G610_X2, G610_X0);
                e.shufps(G610_X2, G610_X2, 0x39u);   // _MM_SHUFFLE(0,3,2,1)
                break;
            default:
                *why = G610_RF_UPPER_CLS;
                return false;
            }
            // None of these five writes MAC/STATUS or the G430 history; all write VF[ft].
            e.storeDest(G610_X2, G610_RCX, G610_OFF_VF + (int32_t)(16u * ft), dest,
                        G610_X4, G610_X5);
            return true;
        }

        // cls 1 (-> VF[fd]) / cls 2 (-> ACC). Operand b:
        switch (d.src)
        {
        case 0u: // broadcast lane bc of vt
            e.movupsLd(G610_X1, G610_RCX, G610_OFF_VF + (int32_t)(16u * ft));
            e.shufps(G610_X1, G610_X1, (uint8_t)(d.bc * 0x55u));
            break;
        case 1u: // Q
            e.movssLd(G610_X1, G610_RCX, G610_OFF_Q);
            e.shufps(G610_X1, G610_X1, 0x00u);
            break;
        case 2u: // I
            e.movssLd(G610_X1, G610_RCX, G610_OFF_I);
            e.shufps(G610_X1, G610_X1, 0x00u);
            break;
        default: // full vt
            e.movupsLd(G610_X1, G610_RCX, G610_OFF_VF + (int32_t)(16u * ft));
            break;
        }

        // Result. Operand ORDER matches the intrinsics in g421FastUpper exactly — it is observable
        // only for NaN inputs, and matching it is what makes this exact rather than equivalent.
        // ⭐ G611: `mulDstIsB` / `outerDstIsMul` come from `g611ProbeOrder`, which MEASURED the
        // shipped `g421FastUpper` for THIS kind. Reading the order off the intrinsic source is what
        // put a wrong NaN payload in the G430 history for every commutative form (§5b).
        const G611OpOrder ord = s_g611Ord[kind];
        switch (d.op)
        {
        case 0u: // _mm_add_ps(vs, b)
            if (ord.mulDstIsB) { e.movaps(G610_X2, G610_X1); e.addps(G610_X2, G610_X0); }
            else               { e.movaps(G610_X2, G610_X0); e.addps(G610_X2, G610_X1); }
            break;
        case 1u: e.movaps(G610_X2, G610_X0); e.subps(G610_X2, G610_X1); break; // _mm_sub_ps(vs,b)
        case 4u: // _mm_mul_ps(vs, b)
            if (ord.mulDstIsB) { e.movaps(G610_X2, G610_X1); e.mulps(G610_X2, G610_X0); }
            else               { e.movaps(G610_X2, G610_X0); e.mulps(G610_X2, G610_X1); }
            break;
        case 5u: e.movaps(G610_X2, G610_X0); e.maxps(G610_X2, G610_X1); break; // _mm_max_ps(vs,b)
        case 6u: e.movaps(G610_X2, G610_X0); e.minps(G610_X2, G610_X1); break; // _mm_min_ps(vs,b)
        case 2u: // _mm_add_ps(acc, _mm_mul_ps(vs, b))
            if (ord.mulDstIsB) { e.movaps(G610_X3, G610_X1); e.mulps(G610_X3, G610_X0); }
            else               { e.movaps(G610_X3, G610_X0); e.mulps(G610_X3, G610_X1); }
            if (ord.outerDstIsMul)
            {
                e.movaps(G610_X2, G610_X3);
                e.addpsM(G610_X2, G610_RCX, G610_OFF_ACC);
            }
            else
            {
                e.movupsLd(G610_X2, G610_RCX, G610_OFF_ACC);
                e.addps(G610_X2, G610_X3);
            }
            break;
        case 3u: // _mm_sub_ps(acc, _mm_mul_ps(vs, b)) — the subtract cannot be commuted
            if (ord.mulDstIsB) { e.movaps(G610_X3, G610_X1); e.mulps(G610_X3, G610_X0); }
            else               { e.movaps(G610_X3, G610_X0); e.mulps(G610_X3, G610_X1); }
            e.movupsLd(G610_X2, G610_RCX, G610_OFF_ACC); e.subps(G610_X2, G610_X3);
            break;
        // G611 — the two CROSS-LANE outer-product forms. `d.src == 3` for both, so X1 already holds
        // the whole `vt`. Both build G488's permutation
        //     a = {vs.y, vs.z, vs.x, vs.w}   bb = {vt.z, vt.x, vt.y, vt.w}
        // (`_MM_SHUFFLE(3,0,2,1)` = 0xC9 and `_MM_SHUFFLE(3,1,0,2)` = 0xD2), which is execUpper's
        // operand pairing lane for lane.
        case 7u: // OPMSUB : ACC - (vs x vt), w lane forced to +0.0
            e.movaps(G610_X2, G610_X0); e.shufps(G610_X2, G610_X2, 0xC9u);   // a
            e.movaps(G610_X3, G610_X1); e.shufps(G610_X3, G610_X3, 0xD2u);   // bb
            if (ord.mulDstIsB) { e.mulps(G610_X3, G610_X2); e.movaps(G610_X2, G610_X3); }
            else               { e.mulps(G610_X2, G610_X3); }                // a * bb
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_ACC);
            e.subps(G610_X3, G610_X2);                                       // acc - (a*bb)
            // `result[3] = 0.0f`: masking w to all-zero bits IS the +0.0f bit pattern for any value
            // the subtract left there, so the destination store sees exactly what execUpper wrote.
            e.andpsM(G610_X3, G610_R11, G610_C_WMASK);
            e.movaps(G610_X2, G610_X3);
            usedG611 = true;
            break;
        case 8u: // OPMULA : (vs x vt) -> ACC, w lane keeps the CURRENT ACC.w
            e.movaps(G610_X2, G610_X0); e.shufps(G610_X2, G610_X2, 0xC9u);   // a
            e.movaps(G610_X3, G610_X1); e.shufps(G610_X3, G610_X3, 0xD2u);   // bb
            if (ord.mulDstIsB) { e.mulps(G610_X3, G610_X2); e.movaps(G610_X2, G610_X3); }
            else               { e.mulps(G610_X2, G610_X3); }                // p = a * bb
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_ACC);                     // acc, read BEFORE
            e.movaps(G610_X4, G610_X2);
            e.unpckhps(G610_X4, G610_X3);                // {p.z, acc.z, p.w, acc.w}
            e.shufps(G610_X2, G610_X4, 0xC4u);           // {p.x, p.y, p.z, acc.w}
            usedG611 = true;
            break;
        default:
            *why = G610_RF_UPPER_OP;
            return false;
        }

        // G430 history push. `historyResult` differs from `r` only for MIN/MAX with a partial dest,
        // where G546 rewrites the MASKED-OFF lanes with NVIDIA's own subnormal-as-signed-zero
        // selection. The active lanes are `r` either way; only dead history lanes move.
        unsigned histReg = G610_X2;
        if ((d.op == 5u || d.op == 6u) && dest != 0xFu)
        {
            if (!g611Ops)
            {
                *why = G610_RF_MINMAX_DEST;
                return false;
            }
            usedG611 = true;
            // flushA -> X3, flushB -> X4 (X5 is the shared second temporary).
            g611EmitFlushSubnormals(e, G610_X0, G610_X3, G610_X5);
            g611EmitFlushSubnormals(e, G610_X1, G610_X4, G610_X5);
            if (d.op == 5u)                              // MAX: chooseA = flushA > flushB
            { e.movaps(G610_X5, G610_X4); e.cmpltps(G610_X5, G610_X3); }
            else                                         // MIN: chooseA = flushA < flushB
            { e.movaps(G610_X5, G610_X3); e.cmpltps(G610_X5, G610_X4); }
            e.movaps(G610_X3, G610_X5);
            e.andps(G610_X3, G610_X0);                   // chooseA & a
            e.andnps(G610_X5, G610_X1);                  // ~chooseA & b
            e.orps(G610_X3, G610_X5);                    // gpuResult
            e.movapsLd(G610_X4, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
            e.movaps(G610_X5, G610_X4);
            e.andps(G610_X5, G610_X2);                   // active & cpuResult
            e.andnps(G610_X4, G610_X3);                  // ~active & gpuResult
            e.orps(G610_X4, G610_X5);                    // historyResult
            histReg = G610_X4;
        }
        usesHist = true;
        // `H.vec[s]` has a 16-byte stride and x86 SIB scales only to 8, so the byte offset is built
        // in two steps: RAX = s*8, then an index scaled by 2. RAX is free here — the stamp value is
        // computed on the next line, after the store. (The oracle caught the scale-1 first version
        // on its very first block: `[G610:verify-BAD] hist.vec at=0`.)
        e.lea64Idx(G610_RAX, G610_R10, 8u, 0);
        e.movupsStXS(histReg, G610_R9, G610_RAX, 2u, G610_OFF_HVEC); // vec[s]
        e.lea64(G610_RAX, G610_R8, (int32_t)pairIndexInBlock);       // stampBase + j
        e.movSt64X(G610_RAX, G610_R9, G610_R10, 8u, G610_OFF_HSTAMP);
        e.movStImm8X(G610_R9, G610_R10, G610_OFF_HDEST, dest);
        e.movStImm8X(G610_R9, G610_R10, G610_OFF_HKIND, G428_MK_PENDING);
        e.incR32(G610_R10);
        e.andImm(G610_R10, 7);

        e.storeDest(G610_X2, G610_RCX,
                    (d.cls == 1u) ? (G610_OFF_VF + (int32_t)(16u * fd)) : G610_OFF_ACC,
                    dest, G610_X4, G610_X5);
        return true;
    }

    // Emit the LOWER slot. Returns false to refuse the whole block.
    bool g610EmitLower(G610Emit &e, uint32_t lower, uint8_t kind, uint32_t dataSize, int *why)
    {
        const unsigned it = LIT(lower), is = LIS(lower), id = LID(lower);
        const uint8_t dest = (uint8_t)((lower >> 21) & 0xFu);
        const int32_t imm11 = (int32_t)IMM11(lower);
        const int32_t immU = (int32_t)(int16_t)((int16_t)(lower & 0x7FFu) |
                                               (int16_t)((lower >> 10) & 0x7800u));
        const int32_t imm5 = (int32_t)(int16_t)((int32_t)(((lower) >> 6) & 0x1Fu) << 27 >> 27);

        switch (kind)
        {
        case G422_LK_NOP:
            return true;

        // ---- integer ALU ----
        case G422_LK_IADDI:
            if (it == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.addImm(G610_RAX, imm5);
            e.storeViTrunc(it);
            return true;
        case G422_LK_IADDIU:
            if (it == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.addImm(G610_RAX, immU);
            e.storeViTrunc(it);
            return true;
        case G422_LK_ISUBIU:
            if (it == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.addImm(G610_RAX, -immU);
            e.storeViTrunc(it);
            return true;
        case G422_LK_IADD:
            if (id == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.addM(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            e.storeViTrunc(id);
            return true;
        case G422_LK_ISUB:
            if (id == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.subM(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            e.storeViTrunc(id);
            return true;
        case G422_LK_IAND:  // no int16 truncation in the shipped body
            if (id == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.andM(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            e.storeViRaw(id);
            return true;
        case G422_LK_IOR:   // ditto
            if (id == 0u) return true;
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.orM(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            e.storeViRaw(id);
            return true;

        // ---- VF <-> VI moves ----
        case G422_LK_MOVE:
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            return true;
        case G422_LK_MR32:
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
            e.shufps(G610_X3, G610_X3, 0x39u);       // {y,z,w,x}
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            return true;
        case G422_LK_MTIR:   // vi[it] = (int16_t)low16(bits(vf[is][comp]))
        {
            if (it == 0u) return true;
            const unsigned comp = (unsigned)((lower >> 21) & 0x3u);
            e.movsx16M(G610_RAX, G610_RCX,
                       G610_OFF_VF + (int32_t)(16u * is) + (int32_t)(4u * comp));
            e.storeViRaw(it);
            return true;
        }
        case G422_LK_MFIR:   // splat the sign-extended low 16 bits of vi[is] as RAW BITS
            e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            e.movsx16R(G610_RAX, G610_RAX);
            e.movdToX(G610_X3, G610_RAX);
            e.shufps(G610_X3, G610_X3, 0x00u);
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            return true;

        // ---- quadword memory ----
        case G422_LK_LQ:
            g610EmitAddrImm(e, is, imm11, dataSize);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            return true;
        case G422_LK_LQI:
            g610EmitAddrU16(e, is, dataSize);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            g610EmitViStep(e, is, +1);
            return true;
        case G422_LK_LQD:
            g610EmitViStep(e, is, -1);               // the decrement happens FIRST
            g610EmitAddrU16(e, is, dataSize);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            e.storeDest(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * it), dest,
                        G610_X4, G610_X5);
            return true;
        case G422_LK_SQ:
            g610EmitAddrImm(e, it, imm11, dataSize);
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
            g610EmitStoreQuadMasked(e, G610_X3, dest);
            return true;
        case G422_LK_SQI:
            g610EmitAddrU16(e, it, dataSize);
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
            g610EmitStoreQuadMasked(e, G610_X3, dest);
            g610EmitViStep(e, it, +1);
            return true;
        case G422_LK_SQD:
            g610EmitViStep(e, it, -1);
            g610EmitAddrU16(e, it, dataSize);
            e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
            g610EmitStoreQuadMasked(e, G610_X3, dest);
            return true;

        // ---- 32-bit integer memory ----
        //
        // ⚠️⚠️ ILW AND ILWR DO NOT USE THE SAME COMPONENT SELECTOR, and the difference is invisible
        // unless both bodies are read side by side. ILW is an ELSE-IF chain, so the HIGHEST-priority
        // set dest bit wins (x > y > z > w). ILWR is a straight IF chain of overwrites into one
        // `value`, so the LOWEST-priority set bit wins (w > z > y > x) — and with `dest == 0` its
        // `value` stays 0 and is STORED. Both are reproduced literally below.
        //
        // Neither needs a second integer scratch: `movsx r32, WORD [rdx+rax+k]` computes the address
        // from RAX and then overwrites RAX with the sign-extended low half, which is exactly
        // `(int32_t)(int16_t)(v & 0xFFFF)`. The two stores park their 16-bit value in an XMM instead.
        case G422_LK_ILW:
        {
            if (it == 0u) return true;               // the shipped body guards the store, not the load
            const unsigned comp = (dest & 0x8u) ? 0u : (dest & 0x4u) ? 1u
                                : (dest & 0x2u) ? 2u : 3u;
            g610EmitAddrImm(e, is, imm11, dataSize);
            e.movsx16MX(G610_RAX, G610_RDX, G610_RAX, (int32_t)(4u * comp));
            e.storeViRaw(it);
            return true;
        }
        case G422_LK_ILWR:
        {
            if (it == 0u) return true;               // ILWR returns before doing anything
            if (dest == 0u)
            {
                e.movImm32(G610_RAX, 0u);            // `value` was never written; 0 is stored
                e.storeViRaw(it);
                return true;
            }
            const unsigned comp = (dest & 0x1u) ? 3u : (dest & 0x2u) ? 2u
                                : (dest & 0x4u) ? 1u : 0u;
            g610EmitAddrU16(e, is, dataSize);
            e.movsx16MX(G610_RAX, G610_RDX, G610_RAX, (int32_t)(4u * comp));
            e.storeViRaw(it);
            return true;
        }
        case G422_LK_ISW:
        case G422_LK_ISWR:
        {
            if (dest == 0u) return true;             // no lane selected: the body stores nothing
            // The stored word is `(uint32_t)(uint16_t)(vi[it] & 0xFFFF)` — upper 16 bits ZERO, not
            // sign-extended. Parked in XMM3 so RAX is free for the address.
            e.movzx16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * it));
            e.movdToX(G610_X3, G610_RAX);
            if (kind == G422_LK_ISW)
                g610EmitAddrImm(e, is, imm11, dataSize);
            else
                g610EmitAddrU16(e, is, dataSize);
            for (unsigned lane = 0u; lane < 4u; ++lane)
                if (dest & (0x8u >> lane))
                    e.movssStX(G610_X3, G610_RDX, G610_RAX, (int32_t)(4u * lane));
            return true;
        }

        default:
            *why = G610_RF_LOWER_KIND;
            return false;
        }
    }

    void g610ArenaInit()
    {
#if defined(_WIN32)
        if (s_g610.arena == nullptr && !s_g610.arenaFull)
        {
            // MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE. The buffer is written and executed
            // by the SAME thread (translation happens at a block entry on the VU1 worker), and x86
            // self-modifying code is architecturally coherent when the writing and executing
            // instructions are separated by a branch — which the `call` into the block provides.
            // FlushInstructionCache is issued anyway, once per compiled block, for good form.
            s_g610.arena = (uint8_t *)VirtualAlloc(nullptr, G610_ARENA_BYTES, 0x1000u | 0x2000u,
                                                   0x40u);
            if (s_g610.arena == nullptr)
                s_g610.arenaFull = true;
        }
#else
        s_g610.arenaFull = true;
#endif
    }

    // Called from g490RebuildRunLengths() — the SAME cold pass, on BOTH invalidation paths, that
    // rebuilds g490RunLen/g500Tail/g516*. Every slot->body binding is dropped, so a body can never
    // be reached for an encoding it was not emitted from; the bodies themselves survive in the
    // (source-word-keyed) code cache and are re-bound by hash below.
    void g610UnbindAll(uint32_t dataSize)
    {
        for (auto &s : s_g610.slot)
            s = G610SlotRec{};
        s_g610.dataSize = dataSize;
    }

    // The arena is a bump allocator with no free list — when it fills, every body is dropped at
    // once and the cache emptied with them. Rare by construction (the live image set is small and
    // each image translates once), and counted so "rare" is a measurement.
    void g610ArenaReset()
    {
        s_g610.used = 0u;
        s_g610.cache.fill(G610CacheEnt{});
        s_g610.cacheUsed = 0u;
        ++s_g610ArenaResets;
        for (auto &s : s_g610.slot)
            s = G610SlotRec{};
    }

    // Translate the block starting at `firstIndex` for `count` pairs. Returns nullptr on refusal;
    // the caller then marks the slot `tried` and never asks again until the next invalidation.
    G610Fn g610TranslateBlock(uint32_t firstIndex, uint32_t count, uint32_t dataSize,
                              bool *readsQ, uint64_t *qReadMask = nullptr,
                              bool *usedG611Out = nullptr, bool allowReset = true)
    {
        *readsQ = false;
        if (qReadMask != nullptr)
            *qReadMask = 0ull;
        if (usedG611Out != nullptr)
            *usedG611Out = false;
        // Precondition 1: the power-of-two data size that makes the interpreter's bounds guard the
        // identity. Re-derived here, not inherited from another phase's constant (G608's law).
        if (dataSize < 16u || (dataSize & (dataSize - 1u)) != 0u)
        {
            ++s_g610RfWhy[G610_RF_DATASIZE];
            return nullptr;
        }
        g610ArenaInit();
        if (s_g610.arena == nullptr)
        {
            ++s_g610RfWhy[G610_RF_ARENA];
            return nullptr;
        }
        if (s_g610.used + G610_MAX_BLOCK_BYTES > G610_ARENA_BYTES)
        {
            // ⚠️ G611: an arena reset UNBINDS every slot and empties the code cache. The Q-split
            // path calls this from inside a live block entry, holding a body pointer of its own, so
            // it passes `allowReset = false` and simply declines rather than pulling the ground out
            // from under its caller.
            if (!allowReset)
            {
                ++s_g610RfWhy[G610_RF_ARENA];
                return nullptr;
            }
            g610ArenaReset();
        }

        G610Emit e;
        e.p = s_g610.arena + s_g610.used;
        e.cap = G610_MAX_BLOCK_BYTES;
        e.n = 0u;

        const G479FusedPair *const fused = s_g410Cache.fused.data();

        // Does any pair in this block push G430 history? Decided in a pre-pass so the prologue can
        // omit the cursor load entirely for a block of pure VNOP uppers.
        bool anyHist = false;
        for (uint32_t j = 0u; j < count; ++j)
            if (fused[firstIndex + j].pair.upperKind != 0x6fu)
            {
                anyHist = true;
                break;
            }
        // Reserve the prologue; if `anyHist`, load the ring cursor once (G504's exactness argument).
        if (anyHist)
            e.movLd32(G610_R10, G610_R9, G610_OFF_HHEAD);
        e.movImm64(G610_R11, (uint64_t)(uintptr_t)&g610Consts);

        int why = G610_RF_ENCODE;
        bool usesHist = false;
        bool usedG611 = false;
        for (uint32_t j = 0u; j < count; ++j)
        {
            const G479FusedPair &fp = fused[firstIndex + j];
            const uint32_t upper = (uint32_t)(fp.source >> 32);
            const uint32_t lower = (uint32_t)fp.source;

            // Does this block OBSERVE `st.q`? Only an upper with `src == 1` (MULq/ADDq/MADDq/...)
            // does; nothing in the compiled lower set reads Q, and nothing anywhere in it reads P
            // (MFP decodes to G422_LK_NONE, which G490 never admits). This is what lets the entry
            // publish an unobservable mid-block Q/P instead of refusing the block — see
            // `g610QpSingleSegment`.
            if (fp.pair.upperKind != 0x6fu &&
                g421DescTableConst.e[fp.pair.upperKind].cls != 0u &&
                g421DescTableConst.e[fp.pair.upperKind].cls < 3u &&
                g421DescTableConst.e[fp.pair.upperKind].src == 1u)
            {
                *readsQ = true;
                // ⭐ G611 — WHERE the Q readers are, not merely THAT there are some. A publication
                // at index k is observable only by a pair BEFORE it, so a per-pair bitmap turns
                // `g610QpSingleSegment`'s whole-block veto into a prefix test. Pairs at index >= 64
                // are folded into bit 63, which is conservative in the refusing direction.
                if (qReadMask != nullptr)
                    *qReadMask |= 1ull << ((j < 63u) ? j : 63u);
            }

            if (!g610EmitUpper(e, upper, fp.pair.upperKind, j, usesHist, usedG611, &why))
            {
                ++s_g610RfWhy[why];
                ++s_g610Refused;
                s_g610RefusedPairs += count;
                return nullptr;
            }
            if (!g610EmitLower(e, lower, fp.pair.lowerKind, dataSize, &why))
            {
                ++s_g610RfWhy[why];
                ++s_g610Refused;
                s_g610RefusedPairs += count;
                return nullptr;
            }
            // G425: re-pin VF0 = (0,0,0,1) and VI0 = 0 exactly where the generic body does, and
            // only when the same precomputed bit says a writer of this pair can reach register 0.
            if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                e.movapsLd(G610_X4, G610_R11, G610_C_VF0);
                e.movupsSt(G610_X4, G610_RCX, G610_OFF_VF);
                e.movStImm32(G610_RCX, G610_OFF_VI, 0u);
            }
            if (e.bad)
                break;
        }
        if (anyHist && usesHist)
            e.movSt32(G610_R10, G610_R9, G610_OFF_HHEAD);
        e.ret();

        if (e.bad)
        {
            ++s_g610RfWhy[G610_RF_ENCODE];
            ++s_g610Refused;
            return nullptr;
        }

        if (usedG611Out != nullptr)
            *usedG611Out = usedG611;
        G610Fn fn = (G610Fn)(void *)e.p;
        // 16-byte-align the next block so no two bodies share a hot cache line boundary.
        s_g610.used += (e.n + 15u) & ~(size_t)15u;
        s_g610CodeBytes += e.n;
        ++s_g610Compiled;
#if defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), e.p, e.n);
#endif
        return fn;
    }

    // G611 — the SINGLE entry point to the source-word code cache, shared by the cold rebuild pass
    // and by the Q-split path. Returns the entry for (firstIndex, count), translating on a miss;
    // `fn == nullptr` in a returned entry is a cached NEGATIVE (this exact encoding cannot be
    // translated) and must not be retried. Returns nullptr only if the cache line is full.
    G610CacheEnt *g610CacheLookup(uint32_t firstIndex, uint32_t count, uint32_t dataSize,
                                  bool allowReset)
    {
        const uint64_t h = g610HashBlock(firstIndex, count);
        const size_t probe = (size_t)(h & (G610_CACHE_SLOTS - 1u));
        for (size_t k = 0u; k < 64u; ++k)
        {
            G610CacheEnt &ce = s_g610.cache[(probe + k) & (G610_CACHE_SLOTS - 1u)];
            if (ce.key == h && ce.count == (uint16_t)count)
            {
                ++s_g610CacheHit;
                return &ce;
            }
            if (ce.key == 0u)
            {
                ++s_g610CacheMiss;
                bool readsQ = false, usedG611 = false;
                uint64_t qMask = 0ull;
                G610Fn fn = g610TranslateBlock(firstIndex, count, dataSize, &readsQ, &qMask,
                                               &usedG611, allowReset);
                // g610TranslateBlock may have reset the arena (and with it this cache), so the
                // reference above can no longer be trusted — re-probe to insert.
                const size_t p2 = (size_t)(h & (G610_CACHE_SLOTS - 1u));
                for (size_t m = 0u; m < 64u; ++m)
                {
                    G610CacheEnt &ins = s_g610.cache[(p2 + m) & (G610_CACHE_SLOTS - 1u)];
                    if (ins.key == 0u || (ins.key == h && ins.count == (uint16_t)count))
                    {
                        if (ins.key == 0u)
                            ++s_g610.cacheUsed;
                        ins.key = h;
                        ins.fn = fn;
                        ins.count = (uint16_t)count;
                        ins.readsQ = readsQ ? 1u : 0u;
                        ins.g611 = usedG611 ? 1u : 0u;
                        ins.qMask = qMask;
                        return &ins;
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }

    // -------------------------------------------------------------------------------------------
    // §7 THE ORACLE (DC2_G610_VERIFY=1).
    //
    // A SAME-RUN oracle with the SHIPPED GENERIC PATH AUTHORITATIVE: snapshot the whole
    // architectural state, the whole G430 ring and all of VU data RAM; run the compiled body; keep
    // its result; restore the snapshot; run the generic block executor; compare. Every field of
    // VU1State, all 8 history entries and all `dataSize` bytes of VU RAM are compared, so a wrong
    // emitted byte cannot hide in a register the route happens not to read again.
    //
    // The comparison is on RAW BITS, not floats: `==` on float would call NaN != NaN and a
    // -0.0/+0.0 mix equal, and both of those are exactly the classes an emitted-operand-order
    // mistake produces.
    //
    // ⛔ NEVER in a timing run: it executes every block TWICE and copies 16 KB per block.
    // -------------------------------------------------------------------------------------------
    struct G610Snap
    {
        VU1State st{};
        G430HistBlock hb{};
        int qd = 0, pd = 0;
        std::vector<uint8_t> ram;
    };
    thread_local G610Snap s_g610SnapPre, s_g610SnapJit;

    void g610VerifyFail(const char *what, uint32_t firstIndex, uint32_t count, long where)
    {
        std::fprintf(stderr,
                     "[G610:verify-BAD] %s pc=0x%x count=%u at=%ld afterOk=%llu\n",
                     what, firstIndex * 8u, count, where,
                     (unsigned long long)s_g610Verified);
        // G611: NAME THE PAIR. A whole-state oracle says a block is wrong; it does not say which of
        // the twelve pairs did it, and with three new op forms in flight "which form" is the whole
        // question. Dump the block's decoded kinds and the descriptor of every upper.
        const G479FusedPair *const fused = s_g410Cache.fused.data();
        const VU1State &pre = s_g610SnapPre.st;
        for (uint32_t j = 0u; j < count; ++j)
        {
            const G479FusedPair &fp = fused[firstIndex + j];
            const uint32_t up = (uint32_t)(fp.source >> 32);
            const G421UpperDesc d = g421DescTableConst.e[fp.pair.upperKind];
            const unsigned fs = FS(up), ft = FT(up), fd = FD(up);
            const uint32_t *const pfs = (const uint32_t *)pre.vf[fs];
            const uint32_t *const pft = (const uint32_t *)pre.vf[ft];
            std::fprintf(stderr,
                         "[G610:verify-BAD]   j=%2u src=%016llx upK=0x%02x lowK=%u "
                         "cls=%u op=%u usrc=%u bc=%u dest=0x%x g425=0x%x fs=%u ft=%u fd=%u\n"
                         "[G610:verify-BAD]        pre vf[%2u]=%08x %08x %08x %08x  "
                         "vf[%2u]=%08x %08x %08x %08x\n",
                         j, (unsigned long long)fp.source, (unsigned)fp.pair.upperKind,
                         (unsigned)fp.pair.lowerKind, (unsigned)d.cls, (unsigned)d.op,
                         (unsigned)d.src, (unsigned)d.bc, (unsigned)DEST(up),
                         (unsigned)fp.pair.g425Flags, fs, ft, fd,
                         fs, pfs[0], pfs[1], pfs[2], pfs[3],
                         ft, pft[0], pft[1], pft[2], pft[3]);
        }
        std::fflush(stderr);
        std::abort();
    }

    void g610VerifyPass()
    {
        const uint64_t n = ++s_g610Verified;
        if (n == 1u || (n % 2000000u) == 0u)
        {
            std::fprintf(stderr, "[G610:verify] blocks=%llu bad=0 pairs=%llu\n",
                         (unsigned long long)n, (unsigned long long)s_g610Pairs);
            std::fflush(stderr);
        }
    }

    // Raw-bit comparison of everything a block can write.
    bool g610StatesEqual(const VU1State &a, const VU1State &b, const G430HistBlock &ha,
                         const G430HistBlock &hb, const uint8_t *ra, const uint8_t *rb,
                         uint32_t ramBytes, const char **what, long *at)
    {
        if (std::memcmp(a.vf, b.vf, sizeof(a.vf)) != 0)
        {
            for (int r = 0; r < 32; ++r)
                if (std::memcmp(a.vf[r], b.vf[r], 16) != 0)
                { *what = "vf"; *at = r; return false; }
        }
        if (std::memcmp(a.vi, b.vi, sizeof(a.vi)) != 0)
        {
            for (int r = 0; r < 16; ++r)
                if (a.vi[r] != b.vi[r]) { *what = "vi"; *at = r; return false; }
        }
        if (std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0) { *what = "acc"; *at = -1; return false; }
        if (std::memcmp(&a.q, &b.q, 4) != 0) { *what = "q"; *at = -1; return false; }
        if (std::memcmp(&a.p, &b.p, 4) != 0) { *what = "p"; *at = -1; return false; }
        if (std::memcmp(&a.i, &b.i, 4) != 0) { *what = "i"; *at = -1; return false; }
        if (a.pc != b.pc) { *what = "pc"; *at = -1; return false; }
        if (a.mac != b.mac) { *what = "mac"; *at = -1; return false; }
        if (a.clip != b.clip) { *what = "clip"; *at = -1; return false; }
        if (a.status != b.status) { *what = "status"; *at = -1; return false; }
        if (ha.head != hb.head) { *what = "hist.head"; *at = -1; return false; }
        for (int k = 0; k < G430_HIST; ++k)
        {
            if (std::memcmp(ha.vec[k], hb.vec[k], 16) != 0)
            { *what = "hist.vec"; *at = k; return false; }
            if (ha.stamp[k] != hb.stamp[k]) { *what = "hist.stamp"; *at = k; return false; }
            if (ha.dest[k] != hb.dest[k]) { *what = "hist.dest"; *at = k; return false; }
            if (ha.kind[k] != hb.kind[k]) { *what = "hist.kind"; *at = k; return false; }
        }
        for (uint32_t k = 0u; k < ramBytes; ++k)
            if (ra[k] != rb[k]) { *what = "vuram"; *at = (long)k; return false; }
        return true;
    }

    // -------------------------------------------------------------------------------------------
    // §8 REPORTING
    // -------------------------------------------------------------------------------------------
    void g610Report()
    {
        if (!g610StatOn())
            return;
        // ⚠️ THE DENOMINATOR IS `[G490:block] pairs` FROM THE SAME RUN, not anything printed here.
        // Arm `DC2_G490_STAT=1` beside this flag and divide: the pair loop's own census counts every
        // block on every arm (its `g490NoteBlock` call sits BELOW the whole dispatch chain), so
        // `[G490:block] cover/avg` is the invariant that proves the two A/B arms did the same work.
        std::fprintf(stderr,
                     "[G610:jit] compiled=%llu refused=%llu refusedPairs=%llu codeKiB=%.1f | "
                     "nativeBlocks=%llu nativePairs=%llu avg=%.2f\n",
                     (unsigned long long)s_g610Compiled, (unsigned long long)s_g610Refused,
                     (unsigned long long)s_g610RefusedPairs,
                     (double)s_g610CodeBytes / 1024.0,
                     (unsigned long long)s_g610Blocks, (unsigned long long)s_g610Pairs,
                     s_g610Blocks ? (double)s_g610Pairs / (double)s_g610Blocks : 0.0);
        std::fprintf(stderr,
                     "[G610:cache] rebuilds=%llu hit=%llu miss=%llu live=%zu arenaResets=%llu "
                     "arenaKiB=%.1f\n",
                     (unsigned long long)s_g610Rebuilds, (unsigned long long)s_g610CacheHit,
                     (unsigned long long)s_g610CacheMiss, s_g610.cacheUsed,
                     (unsigned long long)s_g610ArenaResets, (double)s_g610.used / 1024.0);
        std::fprintf(stderr, "[G610:why] fbSeg=%llu fbSegPairs=%llu fbTail=%llu fbNone=%llu "
                             "fbCount=%llu |",
                     (unsigned long long)s_g610FbSeg, (unsigned long long)s_g610FbSegPairs,
                     (unsigned long long)s_g610FbTail,
                     (unsigned long long)s_g610FbNone, (unsigned long long)s_g610FbCount);
        for (int k = 0; k < G610_RF_COUNT; ++k)
            std::fprintf(stderr, " %s=%llu", kG610RfName[k],
                         (unsigned long long)s_g610RfWhy[k]);
        std::fprintf(stderr, "\n");
        // G611 coverage: which of the two new levers is carrying which population, in PAIRS.
        std::fprintf(stderr,
                     "[G611:cov] opsBlocks=%llu opsPairs=%llu | splitBlocks=%llu splitPairs=%llu "
                     "splitFail=%llu\n",
                     (unsigned long long)s_g611OpsBlocks, (unsigned long long)s_g611OpsPairs,
                     (unsigned long long)s_g611SplitBlocks, (unsigned long long)s_g611SplitPairs,
                     (unsigned long long)s_g611SplitFail);
        std::fflush(stderr);
    }

    // The dest-mask cross-check promised in §3: the emitted blend must use bit-for-bit the mask
    // the interpreter's own bit-select uses. Run once, cheaply, at the first translation.
    void g610CheckMaskTable()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        // G611: measure the shipped operand order before a single byte is emitted (§5b).
        g611ProbeOrder();
        if (g610StatOn())
        {
            unsigned nSwap = 0u, nOuter = 0u, nOps = 0u;
            for (unsigned k = 0u; k < 0x100u; ++k)
            {
                const G421UpperDesc d = g421DescTableConst.e[k];
                if (d.cls != 1u && d.cls != 2u)
                    continue;
                ++nOps;
                nSwap += s_g611Ord[k].mulDstIsB;
                nOuter += s_g611Ord[k].outerDstIsMul;
            }
            std::fprintf(stderr,
                         "[G611:order] fmacKinds=%u mulDstIsB=%u maddOuterDstIsMul=%u\n",
                         nOps, nSwap, nOuter);
            std::fflush(stderr);
        }
        for (unsigned d = 0u; d < 16u; ++d)
            for (unsigned lane = 0u; lane < 4u; ++lane)
                if (g610Consts.dest[d][lane] != g421MaskTableConst.m[d][lane])
                {
                    std::fprintf(stderr, "[G610:verify-BAD] dest mask d=%u lane=%u %08x != %08x\n",
                                 d, lane, g610Consts.dest[d][lane],
                                 g421MaskTableConst.m[d][lane]);
                    std::fflush(stderr);
                    std::abort();
                }
        // G611: the same argument for CLIPw's interleave table. A route that never produces a given
        // (gt, lt) pair would let a wrong byte through the whole-state oracle unseen, so the table
        // is checked as a TABLE rather than sampled by execution.
        for (unsigned k = 0u; k < 64u; ++k)
            if (g610Consts.clipTab[k] != g488ClipTableConst.e[k])
            {
                std::fprintf(stderr, "[G610:verify-BAD] clip table k=%u %02x != %02x\n", k,
                             (unsigned)g610Consts.clipTab[k], (unsigned)g488ClipTableConst.e[k]);
                std::fflush(stderr);
                std::abort();
            }
    }
} // namespace

#endif // DC2_G610_JIT_BACKEND
