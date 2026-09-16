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

    // G726: these are now ENTRY-RELATIVE displacements against a base of `R9 + slot*32`, not
    // field-array bases. `G610_OFF_HENT` is the entry stride and the emitter's SIB scale is chosen
    // from it; the static_asserts below are what make the change safe, because the emitted
    // addressing is checked against the C++ layout at compile time rather than by inspection.
    constexpr int32_t G610_OFF_HENT = 32;    // sizeof(G430HistEntry)
    constexpr int32_t G610_OFF_HVEC = 0;     // G430HistEntry::vec[4]
    constexpr int32_t G610_OFF_HSTAMP = 16;  // ::stamp
    constexpr int32_t G610_OFF_HDEST = 28;   // ::dest
    constexpr int32_t G610_OFF_HKIND = 29;   // ::kind  (dest+1: one 16-bit store covers both)
    constexpr int32_t G610_OFF_HHEAD = 256;  // G430HistBlock::head

    static_assert(sizeof(G430HistEntry) == (size_t)G610_OFF_HENT, "G610: hist entry stride");
    // ⭐ G746 PART 4: the whole ring is exactly 256 bytes, which is what makes an 8-bit add the
    // ring wrap. If either constant ever moves, this is a BUILD ERROR and not a silently wrong
    // cursor.
    static_assert(G430_HIST * G610_OFF_HENT == 256, "G746: ring byte-wrap needs a 256-byte ring");
    static_assert(offsetof(G430HistEntry, vec) == (size_t)G610_OFF_HVEC, "G610: hist vec");
    static_assert(offsetof(G430HistEntry, stamp) == (size_t)G610_OFF_HSTAMP, "G610: hist stamp");
    static_assert(offsetof(G430HistEntry, dest) == (size_t)G610_OFF_HDEST, "G610: hist dest");
    static_assert(offsetof(G430HistEntry, kind) == (size_t)G610_OFF_HKIND, "G610: hist kind");
    static_assert(G610_OFF_HKIND == G610_OFF_HDEST + 1, "G610: fused dest/kind store");
    static_assert(offsetof(G430HistBlock, e) == 0u, "G610: hist entries at block base");
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
        G610Fn fn;    // nullptr = no compiled body for this block start
        uint16_t count;    // pairs this body executes; must equal the caller's `count`
        uint8_t readsQ;    // 1 = some upper in this block reads st.q (desc src == 1)
        uint8_t g611;      // 1 = translation needed a G611 op form (the `vuops` arm gates it)
        // G611: WHICH pairs read st.q, bit j = pair j (indices >= 63 fold into bit 63). A mid-block
        // publication at index k is observable only by a reader BEFORE k, so this turns a
        // whole-block veto into a prefix test.
        uint64_t qMask;
        // G611: the one-entry SPLIT memo. `qDelay` at a given block start is a property of where the
        // DIV sits relative to it, so it is stable per slot and a single entry hits.
        G610Fn splitFn1;
        G610Fn splitFn2;
        uint16_t splitN1;  // 0 = memo empty
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
        uint64_t key;   // 0 = empty
        G610Fn fn;
        uint16_t count;
        uint8_t readsQ;
        uint8_t g611;
        uint64_t qMask;
    };

    struct G610Backend
    {
        uint8_t *arena;
        size_t used;
        bool arenaFull;
        uint32_t dataSize;
        // 2,048 x 16 B, read ONCE PER BLOCK (~160 k/frame against ~1.6 M pairs), never per pair —
        // so this is not the per-pair L1D pressure G516 v1 was refuted on.
        G756DirectTable<G610SlotRec, PS2_VU1_CODE_SIZE / 8u> slot;
        std::array<G610CacheEnt, G610_CACHE_SLOTS> cache;
        size_t cacheUsed;
    };
    // This is entirely constant-initializable. State the guarantee so accesses do not need
    // MSVC's on-demand TLS initialization guards in every native block entry.
    constinit thread_local G610Backend s_g610{};

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

    const bool s_g674G610StatOn = f50_12_vu_env_flag("DC2_G610_STAT");
    const bool s_g674G610VerifyOn = f50_12_vu_env_flag("DC2_G610_VERIFY");
    // G747: the successful-block counters also clock independent emitter reports and the
    // oracle. Sample their union once; disabled diagnostics must not write TLS per block.
    const bool s_g747KeepVuCounters = f50_12_vu_env_flag("DC2_G747_KEEP_VU_COUNTERS");
    // Keep oracle snapshots and diagnostic-only spills out of every production native entry.
    const bool s_g751ColdNative = f50_12_vu_env_flag("DC2_G751_COLD_NATIVE") &&
        !f50_12_vu_env_flag("DC2_G751_NO_COLD_NATIVE");
    const bool s_g747NativeTail = f50_12_vu_env_flag("DC2_G747_NATIVE_TAIL") &&
        !f50_12_vu_env_flag("DC2_G747_NO_NATIVE_TAIL");
    const bool s_g747TailStat = f50_12_vu_env_flag("DC2_G747_TAIL_STAT");
    const bool s_g747TailSpecialized = !f50_12_vu_env_flag("DC2_G747_TAIL_GENERIC");
    const bool s_g747RetainBindings = f50_12_vu_env_flag("DC2_G747_RETAIN_BINDINGS") &&
        !f50_12_vu_env_flag("DC2_G747_NO_RETAIN_BINDINGS");
    const bool s_g747BindStat = f50_12_vu_env_flag("DC2_G747_BIND_STAT");
    constinit thread_local std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> s_g747BoundWords{};
    constinit thread_local std::array<uint16_t, PS2_VU1_CODE_SIZE / 8u + 1u> s_g747ChangedPrefix{};
    thread_local bool s_g747BoundValid = false;
    thread_local uint32_t s_g747BoundDecode = 0u;
    thread_local uint64_t s_g747BindingsKept = 0u, s_g747BindingsProbed = 0u;
    struct G747BindingImage
    {
        bool valid;
        uint32_t dataSize, decode, boundCount;
        uint64_t arenaEpoch;
        std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> words;
        G756DirectTable<G610SlotRec, PS2_VU1_CODE_SIZE / 8u> slots;
    };
    constinit thread_local std::array<G747BindingImage, 4> s_g747BindImages{};
    thread_local int s_g747ActiveImage = -1;
    thread_local uint32_t s_g747NextImage = 0u;
    thread_local uint64_t s_g747BindImageHits = 0u;
    thread_local uint64_t s_g747TailRuns = 0u, s_g747TailPairs = 0u;
    const bool s_g747G610Accounting = s_g674G610StatOn || s_g674G610VerifyOn ||
        f50_12_vu_env_flag("DC2_G744_HIST_STAT") ||
        f50_12_vu_env_flag("DC2_G746_DEST_STAT") || s_g747KeepVuCounters || s_g747TailStat;
    void g747TailReport()
    {
        if (s_g747TailStat)
            std::fprintf(stderr, "[G747:tail] enabled=%d runs=%llu pairs=%llu specialized=%d\n",
                         s_g747NativeTail ? 1 : 0, (unsigned long long)s_g747TailRuns,
                         (unsigned long long)s_g747TailPairs, s_g747TailSpecialized ? 1 : 0);
    }
    DC2_G674_HOT_FLAG_INLINE bool g610StatOn() { return s_g674G610StatOn; }
    DC2_G674_HOT_FLAG_INLINE bool g610VerifyOn() { return s_g674G610VerifyOn; }
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

    // G746: SSE4.1 detection, the dest -> BLENDPS immediate table and the dest-shape census. Must
    // precede the encoder, whose `storeDest` reads both.
#include "vu1_g746_blend_dest.inc"

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

        // ---- G746 VEX PREFIX ----
        // `reg` is the ModRM.reg operand, `vvvv` the non-destructive source (pass 0 for the forms
        // that have none — VEX encodes vvvv INVERTED, so 0 becomes the required 1111), `idx`/`base`
        // the ModRM/SIB registers. L is ALWAYS 0: this emitter never issues a 256-bit operation,
        // which is precisely why a VEX body cannot leave a dirty upper YMM behind it.
        void vex(unsigned mm, unsigned pp, unsigned reg, unsigned vvvv, unsigned idx, unsigned base)
        {
            const unsigned v = (~vvvv) & 0xFu;
            if (mm == G746_M_0F && idx < 8u && base < 8u)
            {
                u8(0xC5);
                u8((uint8_t)(((reg >= 8u) ? 0u : 0x80u) | (v << 3) | pp));
                return;
            }
            u8(0xC4);
            u8((uint8_t)(((reg >= 8u) ? 0u : 0x80u) | ((idx >= 8u) ? 0u : 0x40u) |
                         ((base >= 8u) ? 0u : 0x20u) | mm));
            u8((uint8_t)((v << 3) | pp));            // W = 0, L = 0
        }
        void vexRR(uint8_t op, unsigned mm, unsigned pp, unsigned xd, unsigned vvvv, unsigned rm)
        { vex(mm, pp, xd, vvvv, 0u, rm); u8(op); modRR(xd, rm); }
        void vexRM(uint8_t op, unsigned mm, unsigned pp, unsigned xd, unsigned vvvv,
                   unsigned base, int32_t d)
        { vex(mm, pp, xd, vvvv, 0u, base); u8(op); memD(xd, base, d); }
        void vexRMX(uint8_t op, unsigned mm, unsigned pp, unsigned xd, unsigned vvvv,
                    unsigned base, unsigned ix, unsigned sc, int32_t d)
        { vex(mm, pp, xd, vvvv, ix, base); u8(op); memX(xd, base, ix, sc, d); }

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

        // ⭐ G746: every XMM form below picks the VEX encoding when the host has AVX. The VEX and
        // legacy encodings of a 128-bit op compute bit-for-bit the same thing; the only difference
        // is that VEX zeroes bits 255:128 of the destination, which nothing here reads. Routing the
        // NON-destructive `mov*` forms through VEX too is what keeps the emitted body 100 % VEX, so
        // it can never leave a dirty upper half for the host's legacy-SSE code.
        bool avx() const { return m_avx; }
        bool m_avx = g746AvxOn();
        // `m_fold` separates the two halves of this lever: `m_avx` alone re-encodes each existing
        // instruction, `m_fold` also CHANGES THE SHAPE (drops a copy, folds a load). Under
        // `DC2_G746_NO_FOLD=1` the body is VEX but has exactly the instruction COUNT the SSE emitter
        // had, so a gate can price the prefix apart from the count.
        bool m_fold = g746FoldOn();
        // `m_opfold` is the third and last half: naming an operand's MEMORY location as the rm
        // source instead of loading it first. Separately disarmable (`DC2_G746_NO_OPFOLD=1`).
        bool m_opfold = g746OpFoldOn();

        // ⭐⭐ G746 PART 5 — THE ACC CHAIN, WITHOUT A PROLOGUE.
        //
        // A VU1 vertex transform is a RUN of consecutive FMACs into the accumulator
        // (`MULAx / MADDAy / MADDAz / MADDw`), and today each one emits
        //
        //     vaddps  X2, X3, [rcx + ACC]      <- a load that STORE-FORWARDS from the line below
        //     vmovups [rcx + ACC], X2          <- the previous pair's store
        //
        // so consecutive accumulations are chained through the state image. G730 identified exactly
        // this chain, built a whole-body register allocator for it, and measured −0.02 ns/pair —
        // because a BOUND location costs four memory operations per BODY ENTRY (save, load,
        // write-back, restore) and a block entry amortises over 8.4 pairs.
        //
        // ⭐ THIS COSTS NOTHING AT ALL, and that is the whole difference. After
        // `g730StoreAcc(X2, 0xF)` the register X2 already holds bit-for-bit what memory holds, and
        // nothing between that store and the next pair's accumulate touches X2 — `g610EmitLower`
        // never names it, the G430 push writes through R9/R10, the VF0 re-pin uses X4 and the
        // region's budget test uses RAX. So the next pair may simply READ X2 instead of reloading
        // it. No prologue, no epilogue, no write-back set, and no exit is affected because ACC is
        // still stored to the image on EVERY pair exactly as before.
        //
        // ⛔ THE THREE CONDITIONS, each checked and none assumed:
        //   1. the previous ACC write must have been a FULL dest (`0xF`). A partial dest blends in
        //      memory, so the register and the image differ and the reuse would be wrong.
        //   2. ACC must NOT be bound by G730 (then the accessors already name a pool register).
        //   3. the flag is cleared at the start of every basic block, because a BB has predecessors
        //      and there is no phi rule here — the ONE thing G730's whole-body binding existed to
        //      avoid. A straight-line G610 block starts from a fresh `G610Emit`, so it starts false.
        //
        // Rollback `DC2_G746_NO_ACCREG=1`.
        bool m_accInX2 = false;
        int m_vfInX2 = -1; // G747: full VF value, invalid across CFG entries or a write to that VF
        int m_vfInX3 = -1; // lower-slot result/source, consumed before the next upper clobbers X3
        // Translation-time debt only. No guest instruction inside an emitted straight-line
        // span reads the ring head. Materialize before a live push and every control edge.
        unsigned m_histAdvance = 0;

        void flushHistAdvance()
        {
            const unsigned bytes = (m_histAdvance * G610_OFF_HENT) & 255u;
            if (bytes != 0u)
            {
                addImm(G610_R10, (int32_t)bytes);
                andImm(G610_R10, 255);
            }
            m_histAdvance = 0;
        }

        void movupsLd(unsigned x, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x10, G746_M_0F, G746_P_NP, x, 0u, b, d); else sseM(0x10, x, b, d); }
        void movupsStXS(unsigned x, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { if (m_avx) vexRMX(0x11, G746_M_0F, G746_P_NP, x, 0u, b, ix, sc, d);
          else sseMX(0x11, x, b, ix, sc, d); }
        void movupsSt(unsigned x, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x11, G746_M_0F, G746_P_NP, x, 0u, b, d); else sseM(0x11, x, b, d); }
        void movupsLdX(unsigned x, unsigned b, unsigned ix, int32_t d)
        { if (m_avx) vexRMX(0x10, G746_M_0F, G746_P_NP, x, 0u, b, ix, 1u, d);
          else sseMX(0x10, x, b, ix, 1u, d); }
        void movupsStX(unsigned x, unsigned b, unsigned ix, int32_t d)
        { if (m_avx) vexRMX(0x11, G746_M_0F, G746_P_NP, x, 0u, b, ix, 1u, d);
          else sseMX(0x11, x, b, ix, 1u, d); }
        void movapsLd(unsigned x, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x28, G746_M_0F, G746_P_NP, x, 0u, b, d); else sseM(0x28, x, b, d); }
        void movapsStX(unsigned x, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { if (m_avx) vexRMX(0x29, G746_M_0F, G746_P_NP, x, 0u, b, ix, sc, d);
          else sseMX(0x29, x, b, ix, sc, d); }
        void movaps(unsigned xd, unsigned xs)
        { if (m_avx) vexRR(0x28, G746_M_0F, G746_P_NP, xd, 0u, xs); else sseR(0x28, xd, xs); }
        void movssLd(unsigned x, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x10, G746_M_0F, G746_P_F3, x, 0u, b, d); else sseF3M(0x10, x, b, d); }
        void movssSt(unsigned x, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x11, G746_M_0F, G746_P_F3, x, 0u, b, d); else sseF3M(0x11, x, b, d); }
        // ---- G730 additions ----
        // `movss xd, xs` in its REGISTER form (F3 0F 10 /r, mod = 11). Unlike the memory form it
        // MERGES: the low 32 bits come from `xs` and the upper 96 of `xd` survive — which is exactly
        // `applyDest` with dest == 0x8, in one uop and with no memory at all.
        // ⚠️ THE VEX FORM OF THE REGISTER `movss` IS NDS, and its merge source is the FIRST source,
        // not the destination: `vmovss xmm1, xmm2, xmm3` takes the low 32 from xmm3 and the upper 96
        // from xmm2. Naming `xd` as both destination and first source reproduces the legacy merge
        // exactly. (`vvvv = 0` here would silently take the upper 96 from xmm0.)
        void movssR(unsigned xd, unsigned xs)
        { if (m_avx) vexRR(0x10, G746_M_0F, G746_P_F3, xd, xd, xs); else sseF3R(0x10, xd, xs); }
        // `pextrw r32, xmm, imm8` (66 0F C5 /r ib). The reg field is the GPR and the rm field is the
        // XMM — the reverse of `movd`. Word index `2*lane` is the LOW half of lane `lane`, so this
        // plus `movsx r32, r16` is `(int32_t)(int16_t)(bits(vf[is][comp]) & 0xFFFF)` — MTIR's own
        // expression, read out of a RESIDENT register instead of a stale state image.
        void pextrw(unsigned r, unsigned x, uint8_t imm)
        { if (m_avx) { vexRR(0xC5, G746_M_0F, G746_P_66, r, 0u, x); u8(imm); return; }
          u8(0x66); rex(false, r, 0u, x); u8(0x0F); u8(0xC5); modRR(r, x); u8(imm); }
        // movaps [rsp + disp32], xmm  /  movaps xmm, [rsp + disp32].
        // RSP's low three bits are 100, so ModRM rm = 100 means "a SIB byte follows" and `memD`
        // cannot express this base at all. The SIB is index = 100 (none), base = 100 (RSP) = 0x24.
        void movapsRsp(uint8_t op, unsigned x, int32_t d)
        {
            if (m_avx)
                vex(G746_M_0F, G746_P_NP, x, 0u, 0u, 0u);
            else
                rex(false, x, 0u, 0u);
            if (!m_avx)
                u8(0x0F);
            u8(op);
            u8((uint8_t)(0x80u | ((x & 7u) << 3) | 4u)); u8(0x24); i32(d);
        }
        void movapsStRsp(unsigned x, int32_t d) { movapsRsp(0x29, x, d); }
        void movapsLdRsp(unsigned x, int32_t d) { movapsRsp(0x28, x, d); }
        // sub rsp, imm32 / add rsp, imm32 — REX.W 81 /5 and /0 with rm = RSP encoded directly
        // (mod = 11 needs no SIB). Emitted as constants so a wrong byte is a wrong constant here.
        void subImm64Rsp(int32_t v) { u8(0x48); u8(0x81); u8(0xEC); i32(v); }
        void addImm64Rsp(int32_t v) { u8(0x48); u8(0x81); u8(0xC4); i32(v); }
        void movssLdX(unsigned x, unsigned b, unsigned ix, int32_t d)
        { if (m_avx) vexRMX(0x10, G746_M_0F, G746_P_F3, x, 0u, b, ix, 1u, d);
          else sseF3MX(0x10, x, b, ix, d); }
        void movssStX(unsigned x, unsigned b, unsigned ix, int32_t d)
        { if (m_avx) vexRMX(0x11, G746_M_0F, G746_P_F3, x, 0u, b, ix, 1u, d);
          else sseF3MX(0x11, x, b, ix, d); }
        void movdToX(unsigned x, unsigned r)
        { if (m_avx) vexRR(0x6E, G746_M_0F, G746_P_66, x, 0u, r); else sse66R(0x6E, x, r); }

        // ⭐ G746: THE THREE-OPERAND CORE. `ndsR`/`ndsM` emit one VEX instruction `op xd, xs1, src2`
        // where SSE needed `movaps xd, xs1` followed by `op xd, src2`. The two-operand helpers below
        // are exactly `ndsR(op, xd, xd, xs)`, so every existing call site keeps working unchanged and
        // the ones that were emitting the copy can drop it one at a time.
        //
        // ⛔⛔ `xd == xs2` IS A REAL SHAPE AND IT MUST NOT BE REFUSED. The first revision of this
        // helper set `bad` for it "because no call site has that shape" — and OPMULA/OPMSUB under
        // `ord.mulDstIsB` have exactly that shape, so the SSE fallback refused **152 regions and 23
        // blocks** (`[G612:why] encode=152`, `[G610:jit] refused=23`) while the VEX arm refused
        // none. That silently made the ROLLBACK ARM slower than the thing it rolls back to, which
        // is the one defect a rate gate cannot survive: it inflates every candidate reading.
        //
        // The correct SSE sequence keeps the operand ORDER by computing into `xs1` — the destination
        // of a two-operand SSE op IS its first source — and copying down. ⚠️ IT CLOBBERS `xs1`,
        // which is exactly what the two hand-written OPMSUB/OPMULA arms did before G746
        // (`mulps X3, X2` / `movaps X2, X3`), so this is their behaviour generalised, not new.
        void ndsR(uint8_t op, unsigned xd, unsigned xs1, unsigned xs2)
        {
            if (m_avx && (m_fold || xd == xs1))
            { vexRR(op, G746_M_0F, G746_P_NP, xd, xs1, xs2); return; }
            if (xd == xs2 && xd != xs1)
            {
                if (m_avx) { vexRR(op, G746_M_0F, G746_P_NP, xs1, xs1, xs2);
                             vexRR(0x28, G746_M_0F, G746_P_NP, xd, 0u, xs1); }
                else       { sseR(op, xs1, xs2); sseR(0x28, xd, xs1); }
                return;
            }
            if (xd != xs1) { if (m_avx) vexRR(0x28, G746_M_0F, G746_P_NP, xd, 0u, xs1);
                             else sseR(0x28, xd, xs1); }
            if (m_avx) vexRR(op, G746_M_0F, G746_P_NP, xd, xd, xs2);
            else sseR(op, xd, xs2);
        }
        void ndsM(uint8_t op, unsigned xd, unsigned xs1, unsigned b, int32_t d)
        {
            if (m_avx && (m_fold || xd == xs1))
            { vexRM(op, G746_M_0F, G746_P_NP, xd, xs1, b, d); return; }
            if (xd != xs1) { if (m_avx) vexRR(0x28, G746_M_0F, G746_P_NP, xd, 0u, xs1);
                             else sseR(0x28, xd, xs1); }
            if (m_avx) vexRM(op, G746_M_0F, G746_P_NP, xd, xd, b, d);
            else sseM(op, xd, b, d);
        }
        void shufps(unsigned xd, unsigned xs, uint8_t imm)
        { if (m_avx) vexRR(0xC6, G746_M_0F, G746_P_NP, xd, xd, xs); else sseR(0xC6, xd, xs); u8(imm); }
        // ⛔ NO `shufps3`. `shufps`'s immediate takes lanes 0-1 from the DESTINATION and lanes 2-3
        // from the source, so the `xd == xs2` recovery `ndsR` uses (compute into `xs1`, copy down)
        // is NOT semantics-preserving here. The one call site that needs two sources is the
        // two-operand `shufps(xd, xs, imm)` above, which VEX encodes as `vshufps xd, xd, xs, imm`.
        void addps(unsigned xd, unsigned xs) { ndsR(0x58, xd, xd, xs); }
        void mulps(unsigned xd, unsigned xs) { ndsR(0x59, xd, xd, xs); }
        void subps(unsigned xd, unsigned xs) { ndsR(0x5C, xd, xd, xs); }
        void minps(unsigned xd, unsigned xs) { ndsR(0x5D, xd, xd, xs); }
        void maxps(unsigned xd, unsigned xs) { ndsR(0x5F, xd, xd, xs); }
        void andps(unsigned xd, unsigned xs) { ndsR(0x54, xd, xd, xs); }
        void andnps(unsigned xd, unsigned xs) { ndsR(0x55, xd, xd, xs); }
        void orps(unsigned xd, unsigned xs) { ndsR(0x56, xd, xd, xs); }
        void mulpsM(unsigned x, unsigned b, int32_t d) { ndsM(0x59, x, x, b, d); }
        void addpsM(unsigned x, unsigned b, int32_t d) { ndsM(0x58, x, x, b, d); }
        void andpsM(unsigned x, unsigned b, int32_t d) { ndsM(0x54, x, x, b, d); }
        // Three-operand / memory-folded forms the translator names directly.
        void addps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x58, xd, xs1, xs2); }
        void subps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x5C, xd, xs1, xs2); }
        void mulps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x59, xd, xs1, xs2); }
        void minps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x5D, xd, xs1, xs2); }
        void maxps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x5F, xd, xs1, xs2); }
        void andps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x54, xd, xs1, xs2); }
        void andnps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x55, xd, xs1, xs2); }
        void orps3(unsigned xd, unsigned xs1, unsigned xs2) { ndsR(0x56, xd, xs1, xs2); }
        void addps3M(unsigned xd, unsigned xs1, unsigned b, int32_t d) { ndsM(0x58, xd, xs1, b, d); }
        void subps3M(unsigned xd, unsigned xs1, unsigned b, int32_t d) { ndsM(0x5C, xd, xs1, b, d); }
        void mulps3M(unsigned xd, unsigned xs1, unsigned b, int32_t d) { ndsM(0x59, xd, xs1, b, d); }
        void andps3M(unsigned xd, unsigned xs1, unsigned b, int32_t d) { ndsM(0x54, xd, xs1, b, d); }
        void cvtdq2ps(unsigned xd, unsigned xs)
        { if (m_avx) vexRR(0x5B, G746_M_0F, G746_P_NP, xd, 0u, xs); else sseR(0x5B, xd, xs); }
        void cvtdq2psM(unsigned xd, unsigned b, int32_t d)
        { if (m_avx) vexRM(0x5B, G746_M_0F, G746_P_NP, xd, 0u, b, d); else sseM(0x5B, xd, b, d); }
        void cvttps2dq(unsigned xd, unsigned xs)
        { if (m_avx) vexRR(0x5B, G746_M_0F, G746_P_F3, xd, 0u, xs); else sseF3R(0x5B, xd, xs); }

        // ---- G611 SSE additions ----
        void xorps(unsigned xd, unsigned xs) { ndsR(0x57, xd, xd, xs); }
        void xorpsM(unsigned x, unsigned b, int32_t d) { ndsM(0x57, x, x, b, d); }
        void orpsM(unsigned x, unsigned b, int32_t d) { ndsM(0x56, x, x, b, d); }
        void xorps3M(unsigned xd, unsigned xs1, unsigned b, int32_t d) { ndsM(0x57, xd, xs1, b, d); }
        void unpckhps(unsigned xd, unsigned xs) { ndsR(0x15, xd, xd, xs); }
        // cmpps xd, xs, imm8. Predicate 1 = LT, ordered and signalling — the encoding MSVC emits
        // for `_mm_cmplt_ps`, and for `_mm_cmpgt_ps` with the operands exchanged.
        void cmpltps(unsigned xd, unsigned xs)
        { if (m_avx) vexRR(0xC2, G746_M_0F, G746_P_NP, xd, xd, xs); else sseR(0xC2, xd, xs); u8(0x01); }
        // `cmpps`'s destination is its first source, so the `xd == xs2` recovery is the same one
        // `ndsR` uses and it preserves the compare's operand order exactly.
        void cmpltps3(unsigned xd, unsigned xs1, unsigned xs2)
        { if (m_avx) { vexRR(0xC2, G746_M_0F, G746_P_NP, xd, xs1, xs2); u8(0x01); return; }
          if (xd == xs2 && xd != xs1)
          { sseR(0xC2, xs1, xs2); u8(0x01); sseR(0x28, xd, xs1); return; }
          if (xd != xs1) sseR(0x28, xd, xs1);
          sseR(0xC2, xd, xs2); u8(0x01); }
        // pcmpeqd xd, [base+disp]  (66 0F 76 /r) — NDS under VEX.
        void pcmpeqdM(unsigned x, unsigned b, int32_t d)
        { if (m_avx) { vexRM(0x76, G746_M_0F, G746_P_66, x, x, b, d); return; }
          u8(0x66); rex(false, x, 0u, b); u8(0x0F); u8(0x76); memD(x, b, d); }
        void porR(unsigned xd, unsigned xs)
        { if (m_avx) { vexRR(0xEB, G746_M_0F, G746_P_66, xd, xd, xs); return; }
          u8(0x66); rex(false, xd, 0u, xs); u8(0x0F); u8(0xEB); modRR(xd, xs); }
        // movmskps r32, xmm  (0F 50 /r) — reg field is the GPR, rm is the XMM.
        void movmskps(unsigned r, unsigned x)
        { if (m_avx) { vexRR(0x50, G746_M_0F, G746_P_NP, r, 0u, x); return; }
          rex(false, r, 0u, x); u8(0x0F); u8(0x50); modRR(r, x); }
        void movdFromX(unsigned r, unsigned x)
        { if (m_avx) { vexRR(0x7E, G746_M_0F, G746_P_66, x, 0u, r); return; }
          u8(0x66); rex(false, x, 0u, r); u8(0x0F); u8(0x7E); modRR(x, r); }

        // ---- G746 SSE4.1 additions ----
        // `blendps xd, xs, imm8`  (66 0F 3A 0C /r ib) — DEST[i] = imm8[i] ? SRC[i] : DEST[i], lanes
        // numbered from the LOW 32 bits up. One uop, no constant-pool load, no FP semantics at all.
        void blendpsR(unsigned xd, unsigned xs, uint8_t imm)
        { if (m_avx) { vexRR(0x0C, G746_M_0F3A, G746_P_66, xd, xd, xs); u8(imm); return; }
          u8(0x66); rex(false, xd, 0u, xs); u8(0x0F); u8(0x3A); u8(0x0C); modRR(xd, xs); u8(imm); }
        // `vblendps xd, xs1, [b+d], imm8` — AVX only, and the ONE form that folds the destination's
        // old value into the select instead of loading it first.
        void vblendpsM(unsigned xd, unsigned xs1, unsigned b, int32_t d, uint8_t imm)
        { vex(G746_M_0F3A, G746_P_66, xd, xs1, 0u, b); u8(0x0C); memD(xd, b, d); u8(imm); }
        void vblendpsMX(unsigned xd, unsigned xs1, unsigned b, unsigned ix, int32_t d, uint8_t imm)
        { vex(G746_M_0F3A, G746_P_66, xd, xs1, ix, b); u8(0x0C); memX(xd, b, ix, 1u, d); u8(imm); }
        // `vpermilps xd, src, imm8` (VEX.128.66.0F3A.W0 04 /r ib) — AVX only. For a SINGLE-source
        // shuffle it is defined lane for lane exactly as `shufps xd, xd, imm8` with both sources
        // equal, which is the only shape the translator ever emits; unlike `shufps` it takes a
        // separate source, so the broadcast's `movaps`/`movups` disappears (and the memory form
        // folds the load outright).
        void vpermilpsR(unsigned xd, unsigned xs, uint8_t imm)
        { vex(G746_M_0F3A, G746_P_66, xd, 0u, 0u, xs); u8(0x04); modRR(xd, xs); u8(imm); }
        void vpermilpsM(unsigned xd, unsigned b, int32_t d, uint8_t imm)
        { vex(G746_M_0F3A, G746_P_66, xd, 0u, 0u, b); u8(0x04); memD(xd, b, d); u8(imm); }
        // `vbroadcastss xd, [b+d]` (VEX.128.66.0F38.W0 18 /r) — AVX only. Replaces the scalar Q/I
        // operand's `movss` + `shufps` pair with one instruction.
        void vbroadcastssM(unsigned xd, unsigned b, int32_t d)
        { vex(G746_M_0F38, G746_P_66, xd, 0u, 0u, b); u8(0x18); memD(xd, b, d); }

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
        // G726: `C1 /5 ib`. Needed because the ring cursor is now carried PRE-SCALED as a byte
        // offset and has to be converted back to a slot index before it is stored to `head`.
        void shrImm(unsigned r, uint8_t v) { rex(false, 0u, 0u, r); u8(0xC1); modRR(5u, r); u8(v); }
        // ⭐ G746: `add r10b, imm8` (REX.B 80 /0 ib). The G430 ring cursor is carried PRE-SCALED as a
        // byte offset `slot*32` over 8 slots = exactly 0..255, so an EIGHT-BIT add wraps the ring by
        // construction and the separate `and` disappears. Four bytes replace fourteen (`add r32,
        // imm32` + `and r32, imm32`), on every FMAC pair in the corpus.
        //
        // ⚠️ The upper 56 bits of R10 must stay zero, because the SIB index is the FULL register.
        // They are: the prologue writes R10 through a 32-bit `shl`, which zero-extends, and an
        // 8-bit add never carries out of bit 7. The epilogue's `shr r10d, 5` reads the full 32 bits
        // and gets exactly the slot index it always did. There is no partial-register stall here —
        // Intel does not rename the low byte separately since Sandy Bridge, and the instruction
        // already depends on R10's previous value, so the merge is free.
        void addImm8Low(unsigned r, uint8_t v)
        { rex(false, 0u, 0u, r); u8(0x80); modRR(0u, r); u8(v); }
        void movImm32(unsigned r, uint32_t v)
        { rex(false, 0u, 0u, r); u8((uint8_t)(0xB8u + (r & 7u))); u32(v); }
        void movImm64(unsigned r, uint64_t v)
        { rex(true, 0u, 0u, r); u8((uint8_t)(0xB8u + (r & 7u))); u64(v); }
        void movStImm32(unsigned b, int32_t d, uint32_t v)
        { rex(false, 0u, 0u, b); u8(0xC7); memD(0u, b, d); u32(v); }
        void movStImm8X(unsigned b, unsigned ix, int32_t d, uint8_t v)
        { rex(false, 0u, ix, b); u8(0xC6); memX(0u, b, ix, 1u, d); u8(v); }
        // G726: `66 C7 /0 iw` - store a 16-bit immediate through base+index*scale+disp. The 0x66
        // operand-size prefix MUST precede REX (Intel SDM 2.2.1: legacy prefixes come first, and a
        // REX before 0x66 is silently ignored, which would emit a 32-bit store and clobber `mac`).
        // Exists so the adjacent `dest` and `kind` bytes of one history entry are written by ONE
        // store instead of two.
        void movStImm16X(unsigned b, unsigned ix, unsigned sc, int32_t d, uint16_t v)
        { u8(0x66); rex(false, 0u, ix, b); u8(0xC7); memX(0u, b, ix, sc, d); u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
        void movSt64X(unsigned r, unsigned b, unsigned ix, unsigned sc, int32_t d)
        { rex(true, r, ix, b); u8(0x89); memX(r, b, ix, sc, d); }
        void movStX32(unsigned r, unsigned b, unsigned ix, int32_t d)
        { rex(false, r, ix, b); u8(0x89); memX(r, b, ix, 1u, d); }
        void lea64(unsigned rd, unsigned b, int32_t d)
        { rex(true, rd, 0u, b); u8(0x8D); memD(rd, b, d); }
        void incR32(unsigned r) { rex(false, 0u, 0u, r); u8(0xFF); modRR(0u, r); }
        void ret() { u8(0xC3); }

        // ---- composites the translator uses more than once ----

        // ⭐ G746: a SINGLE-SOURCE lane permutation. `shufps xd, xd, imm` and `vpermilps xd, xs, imm`
        // are defined identically when both of `shufps`'s sources are the same register, which is
        // the only shape this translator emits — so the destructive copy is removable outright.
        void shuf1(unsigned xd, unsigned xs, uint8_t imm)
        {
            if (m_fold)
            {
                vpermilpsR(xd, xs, imm);
                return;
            }
            if (xd != xs)
                movaps(xd, xs);
            shufps(xd, xd, imm);
        }
        // Splat lane `l` of xs into all four lanes of xd.
        void splat(unsigned xd, unsigned xs, unsigned l) { shuf1(xd, xs, (uint8_t)(l * 0x55u)); }
        // ⭐ G746: splat the SCALAR at [b+d] into all four lanes — the Q and I operand shape.
        void splatScalarM(unsigned xd, unsigned b, int32_t d)
        {
            if (m_fold)
            {
                vbroadcastssM(xd, b, d);
                return;
            }
            movssLd(xd, b, d);
            shufps(xd, xd, 0x00u);
        }
        // ⭐ G746: splat lane `l` of a VECTOR IN MEMORY. One instruction on AVX — it folds the load
        // the SSE form had to issue separately. This is the dominant VU1 operand shape (`MULx`,
        // `MADDy`, … broadcast one lane of `vt`), so it is the single highest-frequency site in the
        // emitter.
        void splatM(unsigned xd, unsigned b, int32_t d, unsigned l)
        {
            if (s_g752ScalarBc)
            {
                // Read exactly the consumed lane. A 128-bit read after any recent 32-bit VF
                // store can fail forwarding, even when that store changed an unrelated lane.
                // Scalar load+broadcast preserves every bit and has the same SSE instruction
                // count; AVX uses the existing memory VBROADCASTSS form.
                splatScalarM(xd, b, d + (int32_t)(4u * l));
                return;
            }
            if (m_fold)
            {
                vpermilpsM(xd, b, d, (uint8_t)(l * 0x55u));
                return;
            }
            movupsLd(xd, b, d);
            shufps(xd, xd, (uint8_t)(l * 0x55u));
        }

        // `g428StoreDest` / `g422ApplyDest`, dest-shape specialised exactly as G428 ships it.
        // value is in `xv` (preserved); xt1/xt2 are scratch. base+disp is the destination vector.
        void storeDest(unsigned xv, unsigned base, int32_t disp, uint8_t dest,
                       unsigned xt1, unsigned xt2)
        {
            if (g746DestStatOn())
                ++s_g746DestMask[dest & 0xFu];
            if (dest == 0u)
                return;                              // applyDest writes nothing
            if (dest == 0xFu)
            {
                movupsSt(xv, base, disp);             // all four lanes assigned
                return;
            }
            const bool singleLane = (dest & (uint8_t)(dest - 1u)) == 0u;
            // A narrow store followed by a vector load can block store forwarding. The opt-in
            // form merges unchanged bits before one full-width store. VU state is worker-owned;
            // neither form publishes intermediate lane values to another thread.
            const bool wideLane = singleLane && s_g751WideStore;
            if (wideLane && s_g751StoreStat && ++s_g751WideStores == 1)
                std::fprintf(stderr, "[G751:store] wide=1 emitted=1\n");
            if (singleLane && !wideLane)
            {
                const unsigned lane = (dest == 0x8u) ? 0u : (dest == 0x4u) ? 1u
                                    : (dest == 0x2u) ? 2u : 3u;
                if (lane == 0u)
                {
                    movssSt(xv, base, disp);
                }
                else
                {
                    shuf1(xt1, xv, (uint8_t)(lane * 0x55u));
                    movssSt(xt1, base, disp + (int32_t)(4u * lane));
                }
                return;
            }
            // ⭐ G746: genuinely partial. One `blendps` IS the bit-select, lane for lane, with the
            // mask as an immediate instead of two constant-pool loads. Four instructions and two
            // loads fewer than the SSE2 form below, which stays as the fallback for a host without
            // SSE4.1 and as the `DC2_G746_NO_BLEND=1` rollback.
            if (g746BlendOn() || (wideLane && g746HasSse41()))
            {
                if (g746DestStatOn())
                    ++s_g746DestBlendMem;
                if (m_fold)
                {
                    // ⭐ AVX: the old value is the SECOND source, so the load folds into the select
                    // and the whole write is two instructions. The immediate is complemented because
                    // the folded operand is now the "keep" side, not the "take" side.
                    vblendpsM(xt1, xv, base, disp,
                              (uint8_t)(~kG746BlendImm[dest & 0xFu] & 0xFu));
                    movupsSt(xt1, base, disp);
                    return;
                }
                movupsLd(xt1, base, disp);           // xt1 = old
                blendpsR(xt1, xv, kG746BlendImm[dest & 0xFu]);
                movupsSt(xt1, base, disp);
                return;
            }
            if (g746DestStatOn())
                ++s_g746DestLegacy;
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

// ⭐ G730 — THE HOST-REGISTER RESIDENCY ALLOCATOR. Included HERE, inside this file's anonymous
// namespace and immediately after the encoder, because it emits through `G610Emit` and every
// VF/ACC access site below routes through its four accessors. It must come BEFORE §6 (the
// translator) and it is shared verbatim by the G612 region backend, exactly as `g610EmitUpper` is.
#include "vu1_g730_regalloc.inc"

    // Translation-only diagnostic. Dump actual emitted bytes, never a reconstructed instruction
    // list. The caller supplies an existing directory; timing runs leave this variable unset.
    void g752DumpCode(const G610Emit &e, const char *kind)
    {
        static const char *dir = std::getenv("DC2_G752_DUMP_DIR");
        static const unsigned limit = [] {
            const char *v = std::getenv("DC2_G755_DUMP_LIMIT");
            const long n = v ? std::strtol(v, nullptr, 10) : 64;
            return (unsigned)(n > 0 && n <= 4096 ? n : 64);
        }();
        static thread_local unsigned serial = 0;
        if (!dir || e.n <= 16 || serial >= limit) return;
        char path[1024];
        const unsigned id = serial++;
        std::snprintf(path, sizeof(path), "%s/%s_%03u.bin", dir, kind, id);
        if (FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(e.p, 1, e.n, f);
            std::fclose(f);
        }
        std::snprintf(path, sizeof(path), "%s/address_map.tsv", dir);
        if (FILE *f = std::fopen(path, "a"))
        {
            std::fprintf(f, "%p\t%zu\t%s_%03u.bin\n", (void *)e.p, e.n, kind, id);
            std::fclose(f);
        }
    }

// ⭐ G744 — DEAD G430 HISTORY-STORE ELISION. Included here for the same reason G730 is: it is a
// translate-time decision consulted at the push site below and it is shared verbatim by the G612
// region backend. It reads `g421DescTableConst` (available since vu1_g421_fast_upper.inc) and
// `G479FusedPair` / `G430_HIST` (vu1_helpers_and_tables.inc), both already included by this TU.
#include "vu1_g744_hist_elide.inc"

    // -------------------------------------------------------------------------------------------
    // §6 THE TRANSLATOR
    // -------------------------------------------------------------------------------------------

    // `addr = ((uint32_t)(uint16_t)st.vi[r]) * 16u & (dataSize - 1)` into EAX.
    void g610EmitAddrU16(G610Emit &e, unsigned r, uint32_t dataSize, bool viInEax = false)
    {
        if (!viInEax)
            e.movzx16(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * r));
        else if (dataSize > 0x100000u)
            e.andImm(G610_RAX, 0xffff); // Larger RAM masks could retain sign-extension bits.
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
                e.shuf1(G610_X4, xv, (uint8_t)(lane * 0x55u));
                e.movssStX(G610_X4, G610_RDX, G610_RAX, (int32_t)(4u * lane));
            }
            return;
        }
        // ⭐ G746: the SAME seven-instruction bit-select the register path had, on the VU-RAM store
        // path. Same fix, same immediate table — a family is not fixed until every member is.
        if (g746BlendOn())
        {
            if (g746DestStatOn())
                ++s_g746DestBlendMem;
            if (e.m_fold)
            {
                e.vblendpsMX(G610_X4, xv, G610_RDX, G610_RAX, 0,
                             (uint8_t)(~kG746BlendImm[dest & 0xFu] & 0xFu));
                e.movupsStX(G610_X4, G610_RDX, G610_RAX, 0);
                return;
            }
            e.movupsLdX(G610_X4, G610_RDX, G610_RAX, 0);
            e.blendpsR(G610_X4, xv, kG746BlendImm[dest & 0xFu]);
            e.movupsStX(G610_X4, G610_RDX, G610_RAX, 0);
            return;
        }
        if (g746DestStatOn())
            ++s_g746DestLegacy;
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
        return ((const uint32_t *)hb.e[0].vec)[0];
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
        e.andps3M(t1, v, G610_R11, G610_C_ABS);        // absBits
        e.andps3M(t2, t1, G610_R11, G610_C_EXP);       // expBits
        e.pcmpeqdM(t2, G610_R11, G610_C_ZERO);         // expZero
        e.pcmpeqdM(t1, G610_R11, G610_C_ZERO);         // valueZero
        e.andnps(t1, t2);                              // subnormal = ~valueZero & expZero
        e.andps3M(t2, v, G610_R11, G610_C_SIGN);       // signedZero
        e.andps(t2, t1);                               // subnormal & signedZero
        e.andnps(t1, v);                               // ~subnormal & bits
        e.orps(t1, t2);
    }

    // Emit the UPPER slot. Returns false to refuse the whole block.
    bool g610EmitUpper(G610Emit &e, uint32_t upper, uint8_t kind, uint32_t pairIndexInBlock,
                       bool &usesHist, bool &usedG611, int *why)
    {
        // ⭐ G746 PART 5: a VNOP emits nothing at all, so it cannot disturb X2 and the ACC residency
        // survives it. That is not an optimisation detail — VU1 pairs a VNOP upper with a great many
        // lowers, so without this the flag would die on the first `NOP / IADDIU` between two FMACs.
        if (kind == 0x6fu)
            return true;                              // VNOP
        const G421UpperDesc d = g421DescTableConst.e[kind];
        if (d.cls == 0u) { *why = G610_RF_UPPER_CLS; return false; }
        // ⚠️ `m_accInX2` is NOT cleared here — the arms below still have to READ it. Every `return`
        // path sets it to what is true after that path's last emitted instruction.
        const bool g611Ops = g611OpsEmit();
        if (d.cls == 8u && !g611Ops) { *why = G610_RF_UPPER_CLS; return false; }
        if ((d.op == 7u || d.op == 8u) && !g611Ops) { *why = G610_RF_UPPER_OP; return false; }

        const unsigned fs = FS(upper), ft = FT(upper), fd = FD(upper);
        const uint8_t dest = DEST(upper);

        // Operand ORDER matches the intrinsics in g421FastUpper exactly — it is observable only for
        // NaN inputs, and matching it is what makes this exact rather than equivalent.
        // ⭐ G611: `mulDstIsB` / `outerDstIsMul` come from `g611ProbeOrder`, which MEASURED the
        // shipped `g421FastUpper` for THIS kind. Reading the order off the intrinsic source is what
        // put a wrong NaN payload in the G430 history for every commutative form (§5b).
        // ⭐ G746 reads it HERE, before the operand loads, because the order decides WHICH operand
        // sits in the rm position and therefore which one's load can be folded away.
        const G611OpOrder ord = s_g611Ord[kind];

        // ⭐ G746 PART 3 — the operand memory-fold. See vu1_g746_blend_dest.inc for why exactly one
        // of `vs`/`b` is ever eligible and why MIN/MAX and CLIPw are excluded (both read an operand
        // a second time, in the G611 canonicalisation arm and in the clip compare respectively).
        const int32_t g746VsDisp = G610_OFF_VF + (int32_t)(16u * fs);
        const int32_t g746VtDisp = G610_OFF_VF + (int32_t)(16u * ft);
        const bool g746Arith = (d.cls == 1u || d.cls == 2u) &&
                               (d.op == 0u || d.op == 1u || d.op == 2u || d.op == 3u || d.op == 4u);
        const bool g746FoldVs = e.m_opfold && g746Arith && ord.mulDstIsB != 0u &&
                                g730VfReg(fs) < 0 && g747ForwardedVfReg(e, fs) < 0;
        const bool g746FoldB = e.m_opfold && g746Arith && ord.mulDstIsB == 0u &&
                               d.src >= 3u && g730VfReg(ft) < 0 && g747ForwardedVfReg(e, ft) < 0;

        // ⭐ G730: `vs` is READ-ONLY for the whole of this function — every arithmetic form copies
        // it into X2/X3 before modifying anything — so when VF[fs] is resident the load disappears
        // and the arithmetic names the pool register directly. `xVs` is that register; it is X0
        // exactly when the location is not bound, which is the pre-G730 emission byte for byte.
        // ⚠️ When `g746FoldVs` holds, NOTHING reads `xVs` — the arithmetic names `[rcx+g746VsDisp]`
        // directly — so the load is not emitted and the value of `xVs` is never observed.
        const unsigned xVs = g746FoldVs ? G610_X0 : g730SrcVf(e, fs, G610_X0);

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
            const unsigned xVt = g730SrcVf(e, ft, G610_X1);
            e.m_vfInX3 = -1;
            e.splat(G610_X2, xVt, 3u);                  // vt.w splat
            e.andpsM(G610_X2, G610_R11, G610_C_ABS);    // aw = |vt.w|
            e.xorps3M(G610_X3, G610_X2, G610_R11, G610_C_SIGN);   // nw = -|vt.w|
            e.cmpltps3(G610_X4, G610_X2, xVs);          // aw < vs   == vs > aw  (ordered)
            e.cmpltps3(G610_X5, xVs, G610_X3);          // vs < nw               (ordered)
            if (s_g752ClipPack)
            {
                // Preserve ordered float comparisons (including NaNs/denormals), but interleave
                // their booleans before crossing into GPRs. All inputs to signed packing are
                // exactly zero or -1. Low six bytes become +x,-x,+y,-y,+z,-z.
                e.sse66R(0x6B, G610_X4, G610_X4); // packssdw
                e.sse66R(0x6B, G610_X5, G610_X5);
                e.sse66R(0x61, G610_X4, G610_X5); // punpcklwd
                e.sse66R(0x63, G610_X4, G610_X4); // packsswb
                e.sse66R(0xD7, G610_RAX, G610_X4); // pmovmskb
                e.andImm(G610_RAX, 0x3f);
            }
            else
            {
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
            }
            e.shlMemImm(G610_RCX, G610_OFF_CLIP, 6u);
            e.orMemReg(G610_RAX, G610_RCX, G610_OFF_CLIP);
            e.m_accInX2 = false;                      // CLIPw clobbers X2
            e.m_vfInX2 = -1;
            return true;
        }

        if (d.cls >= 3u)
        {
            e.m_vfInX3 = -1;
            // ⭐ G746: `xRes` names the register the result ends up in. MOVE now has NO instruction
            // at all — the value is already in a register and `storeDest` preserves its source, so
            // the copy into X2 was pure loss. Every other arm drops the `movaps` into the
            // three-operand form.
            unsigned xRes = G610_X2;
            switch (d.cls)
            {
            case 3u: // ITOF : (float)bits(vs) * scale     -> VF[ft]
                e.cvtdq2ps(G610_X2, xVs);
                e.mulpsM(G610_X2, G610_R11, G610_C_ITOF + (int32_t)(16u * d.bc));
                break;
            case 4u: // FTOI : bits((int32_t)(vs * scale))  -> VF[ft]
                e.mulps3M(G610_X2, xVs, G610_R11, G610_C_FTOI + (int32_t)(16u * d.bc));
                e.cvttps2dq(G610_X2, G610_X2);
                break;
            case 5u: // ABS
                e.andps3M(G610_X2, xVs, G610_R11, G610_C_ABS);
                break;
            case 6u: // MOVE — the value already IS the result.
                xRes = xVs;
                break;
            case 7u: // MR32 : {vs.y, vs.z, vs.w, vs.x}
                e.shuf1(G610_X2, xVs, 0x39u);        // _MM_SHUFFLE(0,3,2,1)
                break;
            default:
                *why = G610_RF_UPPER_CLS;
                return false;
            }
            // None of these five writes MAC/STATUS or the G430 history; all write VF[ft].
            // ⚠️ `xRes` may be `xVs` (a scratch or a POOL register). `g730StoreVf` preserves its
            // value operand and clobbers only X4/X5, neither of which can be `xVs` — X0/X1 are the
            // operand scratches and a bound location is x6..x15.
            g730StoreVf(e, xRes, ft, dest, G610_X4, G610_X5);
            // ⭐ G746 PART 5: MOVE is the one arm here that emitted NOTHING into X2, so an ACC left
            // there by an earlier pair is still valid. Every other arm wrote X2.
            if (d.cls != 6u)
            {
                e.m_accInX2 = false;
                e.m_vfInX2 = ft != 0u && g730VfReg(ft) < 0 &&
                    ((g747VfForwardOn() && dest == 0xFu) ||
                     (s_g753PartialVf && dest != 0u && dest != 0xFu && e.m_vfInX2 == (int)ft)) ? (int)ft : -1;
            }
            return true;
        }

        // cls 1 (-> VF[fd]) / cls 2 (-> ACC). Operand b:
        //
        // ⭐ G730: `xB` is the register that HOLDS b. The broadcast forms have to write their
        // register (the splat is `shufps xd, xd`), so they always land in the X1 scratch — but a
        // resident vt still saves the LOAD, because `movaps X1, pool` is eliminated at rename while
        // `movups X1, [rcx+…]` is a real load that forwards from whatever last stored that VF.
        // The full-vt form is read-only and can name the pool register itself.
        unsigned xB = G610_X1;
        switch (d.src)
        {
        case 0u: // broadcast lane bc of vt
        {
            // ⭐ G746: the highest-frequency operand shape in the whole emitter. On AVX the load and
            // the broadcast are ONE `vpermilps` against memory; on SSE this is the old
            // load/copy + `shufps` pair, byte for byte.
            const int bvt = g730VfReg(ft);
            if (bvt >= 0)
                e.splat(G610_X1, (unsigned)bvt, d.bc);
            else if (g747ForwardedVfReg(e, ft) >= 0)
            {
                e.splat(G610_X1, (unsigned)g747ForwardedVfReg(e, ft), d.bc);
                ++s_g747VfForwards;
            }
            else
                e.splatM(G610_X1, G610_RCX, G610_OFF_VF + (int32_t)(16u * ft), d.bc);
            break;
        }
        case 1u: // Q — never bound: the hook publishes it between the halves of a G611 Q-split
            e.splatScalarM(G610_X1, G610_RCX, G610_OFF_Q);
            break;
        case 2u: // I — never bound: the G652 LOI path writes it straight into the state image
            e.splatScalarM(G610_X1, G610_RCX, G610_OFF_I);
            break;
        default: // full vt
            // ⚠️ When `g746FoldB` holds, nothing reads `xB` — the arithmetic names
            // `[rcx+g746VtDisp]` directly.
            xB = g746FoldB ? G610_X1 : g730SrcVf(e, ft, G610_X1);
            break;
        }

        // All VF sources now live in X0/X1 or the callee-saved pool. FMACs use X3 for
        // their product; its lower-slot identity must not survive that overwrite.
        e.m_vfInX3 = -1;
        // Save architectural ACC before arithmetic overwrites X2. X4 is otherwise unused by
        // ADD/SUB/MUL/MADD/MSUB until destination materialization. The result history must see
        // the unmasked arithmetic value, so merge only after its history store below.
        const bool g752MergeAcc = s_g752PartialAcc && d.cls == 2u && (d.op <= 4u || d.op == 8u) &&
                                 dest != 0u && dest != 0xFu && g730AccReg() < 0;
        if (g752MergeAcc)
            g730LoadAccInto(e, G610_X4);
        switch (d.op)
        {
        // ⭐ G746: every arm below used to open with a `movaps` that existed ONLY because SSE's
        // arithmetic overwrites its destination. The three-operand form names the same two sources
        // in the same order — `ord` still decides which is which, so the NaN payload G611 MEASURED
        // is unchanged — and drops the copy. On a host without AVX `addps3` re-emits exactly the
        // `movaps` + `addps` pair these lines used to spell.
        case 0u: // _mm_add_ps(vs, b)
            if (ord.mulDstIsB)
            {
                if (g746FoldVs) e.addps3M(G610_X2, xB, G610_RCX, g746VsDisp);
                else            e.addps3(G610_X2, xB, xVs);
            }
            else
            {
                if (g746FoldB) e.addps3M(G610_X2, xVs, G610_RCX, g746VtDisp);
                else           e.addps3(G610_X2, xVs, xB);
            }
            break;
        case 1u: // _mm_sub_ps(vs,b) — never commuted, so only `b` can ever be the folded operand
            if (g746FoldB) e.subps3M(G610_X2, xVs, G610_RCX, g746VtDisp);
            else           e.subps3(G610_X2, xVs, xB);
            break;
        case 4u: // _mm_mul_ps(vs, b)
            if (ord.mulDstIsB)
            {
                if (g746FoldVs) e.mulps3M(G610_X2, xB, G610_RCX, g746VsDisp);
                else            e.mulps3(G610_X2, xB, xVs);
            }
            else
            {
                if (g746FoldB) e.mulps3M(G610_X2, xVs, G610_RCX, g746VtDisp);
                else           e.mulps3(G610_X2, xVs, xB);
            }
            break;
        case 5u: e.maxps3(G610_X2, xVs, xB); break;                            // _mm_max_ps(vs,b)
        case 6u: e.minps3(G610_X2, xVs, xB); break;                            // _mm_min_ps(vs,b)
        case 2u: // _mm_add_ps(acc, _mm_mul_ps(vs, b))
            if (ord.mulDstIsB)
            {
                if (g746FoldVs) e.mulps3M(G610_X3, xB, G610_RCX, g746VsDisp);
                else            e.mulps3(G610_X3, xB, xVs);
            }
            else
            {
                if (g746FoldB) e.mulps3M(G610_X3, xVs, G610_RCX, g746VtDisp);
                else           e.mulps3(G610_X3, xVs, xB);
            }
            // ⭐ G730: with ACC resident this is `addps X2, xmmACC` — the store-to-load forward that
            // chained consecutive FMACs through the state image is gone from the dependency chain.
            if (ord.outerDstIsMul)
            {
                // ⭐ G746: `product + ACC` with the product as the first source — on AVX this is one
                // `vaddps X2, X3, [rcx+ACC]`, folding both the copy and the accumulator load.
                g730AddAccInto3(e, G610_X2, G610_X3);
            }
            else
            {
                g730LoadAccInto(e, G610_X2);
                e.addps(G610_X2, G610_X3);
            }
            break;
        case 3u: // _mm_sub_ps(acc, _mm_mul_ps(vs, b)) — the subtract cannot be commuted
            if (ord.mulDstIsB)
            {
                if (g746FoldVs) e.mulps3M(G610_X3, xB, G610_RCX, g746VsDisp);
                else            e.mulps3(G610_X3, xB, xVs);
            }
            else
            {
                if (g746FoldB) e.mulps3M(G610_X3, xVs, G610_RCX, g746VtDisp);
                else           e.mulps3(G610_X3, xVs, xB);
            }
            g730LoadAccInto(e, G610_X2); e.subps(G610_X2, G610_X3);
            break;
        // G611 — the two CROSS-LANE outer-product forms. `d.src == 3` for both, so X1 already holds
        // the whole `vt`. Both build G488's permutation
        //     a = {vs.y, vs.z, vs.x, vs.w}   bb = {vt.z, vt.x, vt.y, vt.w}
        // (`_MM_SHUFFLE(3,0,2,1)` = 0xC9 and `_MM_SHUFFLE(3,1,0,2)` = 0xD2), which is execUpper's
        // operand pairing lane for lane.
        case 7u: // OPMSUB : ACC - (vs x vt), w lane forced to +0.0
            if (s_g752PartialAcc)
                g730LoadAccInto(e, G610_X5);
            // G747: both outer products reuse X2 for the product BEFORE loading ACC. The
            // architectural store is current, but X2 stops being its cached copy right here.
            e.m_accInX2 = false;
            e.shuf1(G610_X2, xVs, 0xC9u);                                    // a
            e.shuf1(G610_X3, xB, 0xD2u);                                     // bb
            // `mulDstIsB` needs the product's FIRST source to be `bb`, which puts the destination
            // and the second source in the same register — `ndsR` recovers that shape by computing
            // into X3 and copying down, exactly as this arm used to spell by hand.
            if (ord.mulDstIsB) { e.mulps3(G610_X2, G610_X3, G610_X2); }
            else               { e.mulps3(G610_X2, G610_X2, G610_X3); }      // a * bb
            if (s_g752PartialAcc) e.movaps(G610_X3, G610_X5);
            else g730LoadAccInto(e, G610_X3);
            e.subps(G610_X3, G610_X2);                                       // acc - (a*bb)
            // `result[3] = 0.0f`: masking w to all-zero bits IS the +0.0f bit pattern for any value
            // the subtract left there, so the destination store sees exactly what execUpper wrote.
            e.andps3M(G610_X2, G610_X3, G610_R11, G610_C_WMASK);
            usedG611 = true;
            break;
        case 8u: // OPMULA : (vs x vt) -> ACC, w lane keeps the CURRENT ACC.w
            if (s_g752PartialAcc)
                g730LoadAccInto(e, G610_X5);
            e.m_accInX2 = false;
            e.shuf1(G610_X2, xVs, 0xC9u);                                    // a
            e.shuf1(G610_X3, xB, 0xD2u);                                     // bb
            if (ord.mulDstIsB) { e.mulps3(G610_X2, G610_X3, G610_X2); }
            else               { e.mulps3(G610_X2, G610_X2, G610_X3); }      // p = a * bb
            if (s_g752PartialAcc)
                e.blendpsR(G610_X2, G610_X5, 8u);
            else
            {
                g730LoadAccInto(e, G610_X3);
                e.ndsR(0x15, G610_X4, G610_X2, G610_X3);
                e.shufps(G610_X2, G610_X4, 0xC4u);
            }
            usedG611 = true;
            break;
        default:
            *why = G610_RF_UPPER_OP;
            return false;
        }

        // G430 history push. `historyResult` differs from `r` only for MIN/MAX with a partial dest,
        // where G546 rewrites the MASKED-OFF lanes with NVIDIA's own subnormal-as-signed-zero
        // selection. The active lanes are `r` either way; only dead history lanes move.
        // ⭐ G744: is this pair's push DEAD? See vu1_g744_hist_elide.inc. `true` means at least
        // G430_HIST further pushes are guaranteed before any exit from this body, so every ring
        // slot -- including the one this pair would write -- is overwritten before anything can
        // read it, and the three stores below are pure loss.
        const bool g744Elide = g744ElideAt(pairIndexInBlock);
        if (g744HistStatOn())
        {
            ++s_g744HistPushes;
            if (g744Elide)
                ++s_g744HistElided;
        }
        unsigned histReg = G610_X2;
        if ((d.op == 5u || d.op == 6u) && dest != 0xFu)
        {
            // ⚠️ THE REFUSAL RUNS WHETHER OR NOT THE PUSH IS ELIDED. It decides whether the BODY
            // COMPILES, and letting elision change admission would change `pairs/call` -- which a
            // VU1 rate gate is only readable at MATCHED `pairs/call` (G711's rule).
            if (!g611Ops)
            {
                *why = G610_RF_MINMAX_DEST;
                return false;
            }
            usedG611 = true;
            // flushA -> X3, flushB -> X4 (X5 is the shared second temporary). Both operands are
            // read-only here, so a resident `vs`/`vt` feeds the canonicalisation directly.
            // ⭐ G744: this whole arm exists ONLY to build the value the history push stores --
            // ~12 SSE ops on X3/X4/X5, none of which touches X2 (the architectural result) or any
            // architectural state. When the push is elided none of it is needed.
            if (!g744Elide)
            {
                g611EmitFlushSubnormals(e, xVs, G610_X3, G610_X5);
                g611EmitFlushSubnormals(e, xB, G610_X4, G610_X5);
                if (d.op == 5u)                          // MAX: chooseA = flushA > flushB
                    e.cmpltps3(G610_X5, G610_X4, G610_X3);
                else                                     // MIN: chooseA = flushA < flushB
                    e.cmpltps3(G610_X5, G610_X3, G610_X4);
                e.andps3(G610_X3, G610_X5, xVs);         // chooseA & a
                e.andnps(G610_X5, xB);                   // ~chooseA & b
                e.orps(G610_X3, G610_X5);                // gpuResult
                e.movapsLd(G610_X4, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
                e.andps3(G610_X5, G610_X4, G610_X2);     // active & cpuResult
                e.andnps(G610_X4, G610_X3);              // ~active & gpuResult
                e.orps(G610_X4, G610_X5);                // historyResult
                histReg = G610_X4;
            }
        }
        // Matched instruction-throughput control for a latency dose experiment. Both arms
        // emit exactly the same copy and self-inverse shuffles. Only the shuffled register
        // differs: X2 feeds architectural consumers; X5 is dead after this point. Two swaps
        // restore every bit (including NaNs, signed zero and inactive lanes).
        if (s_g753DelayPairs != 0u)
        {
            e.movaps(G610_X5, G610_X2);
            const unsigned delayReg = s_g753DelayIndependent ? G610_X5 : G610_X2;
            for (unsigned n = 0; n != s_g753DelayPairs; ++n)
            {
                e.shufps(delayReg, delayReg, 0xB1);
                e.shufps(delayReg, delayReg, 0xB1);
            }
        }
        usesHist = true;
        // ⭐ G711 measured this push at 0.525 ns/pair = 1.518 ms/f of VU1 busy on `dragon` (7.6% of
        // it) with an order-balanced BALLAST ladder — the largest single term in the compiled pair's
        // memory traffic, three times the operand and ACC round-trips combined. Most of it is DEAD:
        // G612 makes every flag consumer a REGION EXIT, so no pair inside a region can read the
        // history and only the last <=8 FMAC pushes before an exit are live. It is not elided
        // because the prize does not CONVERT — VU1 exceeds every other pole on exactly one of the 23
        // canonical windows, and that window is at the 60 FPS cap (plans/phase-G711-fix-log.md §12).
        // ⭐ G726: ONE entry, ONE cache line, THREE stores (was four, across three lines).
        //
        // The entry stride is 32 and x86 SIB scales only to 8, so the byte offset is built in two
        // steps exactly as before: RAX = s*8, then an index scaled by 4 gives s*32. RAX is free
        // here - the stamp value is computed on the next line, after the vec store. (The oracle
        // caught the scale-1 first version of the OLD addressing on its very first block:
        // `[G610:verify-BAD] hist.vec at=0`. The same oracle covers this change.)
        //
        // `dest` and `kind` are now adjacent bytes inside the entry, so the two `mov byte, imm8`
        // stores fuse into one `mov word, imm16`. Both values are compile-time constants here, so
        // the fusion is a pure encoding change with no extra work to build the immediate.
        // ⚠️ R10 CARRIES THE CURSOR PRE-SCALED, as a BYTE OFFSET `slot*32`, not a slot index.
        // The entry stride is 32 and an x86 SIB scale tops out at 8, so a slot-index cursor would
        // need a `lea` per push to build `slot*32` - and that scratch is RAX, which is already the
        // stamp VALUE by the time the stamp store issues. (That is a real bug, not a hypothetical:
        // the first version of this edit indexed the stamp store at scale 8 off a slot cursor and
        // would have written every stamp to the wrong entry.) Pre-scaling removes the `lea`
        // entirely, so all three stores are plain `[R9 + R10 + disp]` at scale 1 and RAX is free.
        // The prologue shifts left by 5 after loading `head`, the epilogue shifts right by 5 before
        // storing it back, and the wrap mask is 0xFF because 8 entries * 32 B = 256.
        // ⭐ G744: the three stores and the `lea` are what the elision removes. THE CURSOR ADVANCE
        // BELOW IS NOT ELIDED -- keeping it is what makes the ring byte-identical at every exit
        // (the later pushes land in exactly the slots they always did), which is why G612
        // precondition 5 and the `DC2_G610_VERIFY` / `DC2_G612_VERIFY` ring comparison still hold
        // verbatim. It is two ALU ops with no memory traffic.
        if (!g744Elide)
        {
            e.flushHistAdvance();
            e.movupsStXS(histReg, G610_R9, G610_R10, 1u, G610_OFF_HVEC); // e[s].vec
            e.lea64(G610_RAX, G610_R8, (int32_t)pairIndexInBlock);       // stampBase + j
            e.movSt64X(G610_RAX, G610_R9, G610_R10, 1u, G610_OFF_HSTAMP);
            e.movStImm16X(G610_R9, G610_R10, 1u, G610_OFF_HDEST,
                          (uint16_t)((uint16_t)dest | ((uint16_t)G428_MK_PENDING << 8)));
        }
        // ⭐ G746 PART 4: the ring wrap is free. `G430_HIST * G610_OFF_HENT == 256`, so the mask the
        // old form spelled with a 7-byte `and r10d, 255` is exactly what an 8-bit add does for
        // nothing. Two instructions and fourteen bytes become one instruction and four, on the pair
        // form that dominates the corpus — and it is the ONE part of the push G744's elision keeps
        // even when the three stores go, so it is paid by every FMAC whether elided or not.
        if (s_g754DeferredCursor)
        {
            ++e.m_histAdvance;
        }
        else if (g746RingByteOn())
        {
            e.addImm8Low(G610_R10, (uint8_t)G610_OFF_HENT);
        }
        else
        {
            e.addImm(G610_R10, G610_OFF_HENT);
            e.andImm(G610_R10, (G430_HIST * G610_OFF_HENT) - 1);
        }

        // ⭐ G730: cls 1 writes VF[fd], cls 2 writes ACC. When the target is resident this is a
        // register move (full dest), a single `movss` (dest == 0x8) or a pure-ALU blend — the
        // load-modify-store round trip a partial dest costs in the memory form is gone.
        if (g752MergeAcc)
        {
            e.blendpsR(G610_X4, G610_X2, kG746BlendImm[dest]);
            e.movupsSt(G610_X4, G610_RCX, G610_OFF_ACC);
            e.movaps(G610_X2, G610_X4);
        }
        else if (d.cls == 1u)
            g730StoreVf(e, G610_X2, fd, dest, G610_X4, G610_X5);
        else
            g730StoreAcc(e, G610_X2, dest, G610_X4, G610_X5);
        // ⭐ G746 PART 5: X2 now holds the architectural ACC **iff** this pair wrote ACC with a full
        // dest through the memory form. A partial dest blends in memory (register != image) and a
        // bound ACC is already handled by the accessors, so both disqualify.
        e.m_accInX2 = g746AccRegOn() && d.cls == 2u && (dest == 0xFu || g752MergeAcc) && g730AccReg() < 0;
        e.m_vfInX2 = d.cls == 1u && fd != 0u && g730VfReg(fd) < 0 &&
            ((g747VfForwardOn() && dest == 0xFu) ||
             (s_g753PartialVf && dest != 0u && dest != 0xFu && e.m_vfInX2 == (int)fd)) ? (int)fd : -1;
        // ⭐ G746 BALLAST — see vu1_g746_blend_dest.inc. Zero instructions unless `DC2_G746_BAL=N`.
        // X4/X5 are the dest-store scratches and are dead here, so the copies cannot be observed.
        for (uint32_t g746b = g746BallastN(); g746b-- > 0u;)
            e.movaps(G610_X5, G610_X4);
        // ⭐⭐ G746 STORE BALLAST — N extra copies of the destination store just emitted, to the SAME
        // address with the SAME value, so the state is bit-identical and only store-port and
        // store-buffer pressure change. Only the FULL-dest memory form is repeated: a partial dest
        // is a read-modify-write and repeating it would re-read what it just wrote.
        if (g746StoreBallastN() != 0u && dest == 0xFu && g730AccReg() < 0 && g730VfReg(fd) < 0)
        {
            const int32_t g746StDisp = (d.cls == 1u)
                                           ? (G610_OFF_VF + (int32_t)(16u * fd))
                                           : G610_OFF_ACC;
            for (uint32_t g746s = g746StoreBallastN(); g746s-- > 0u;)
                e.movupsSt(G610_X2, G610_RCX, g746StDisp);
        }
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
        {
            // Read-only source, so a resident VF[is] is used in place and MOVE with a full dest
            // between two bound registers becomes ONE `movaps`.
            const unsigned xs = g730SrcVf(e, is, G610_X3);
            g730StoreVf(e, xs, it, dest, G610_X4, G610_X5);
            return true;
        }
        case G422_LK_MR32:
        {
            const int bs = g730VfReg(is);
            if (bs >= 0)
                e.shuf1(G610_X3, (unsigned)bs, 0x39u);   // {y,z,w,x}
            else if (g747ForwardedVfReg(e, is) >= 0)
            {
                e.shuf1(G610_X3, (unsigned)g747ForwardedVfReg(e, is), 0x39u);
                ++s_g747VfForwards;
            }
            else if (e.m_fold)
                e.vpermilpsM(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is), 0x39u);
            else
            {
                e.movupsLd(G610_X3, G610_RCX, G610_OFF_VF + (int32_t)(16u * is));
                e.shufps(G610_X3, G610_X3, 0x39u);       // {y,z,w,x}
            }
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            return true;
        }
        case G422_LK_MTIR:   // vi[it] = (int16_t)low16(bits(vf[is][comp]))
        {
            if (it == 0u) return true;
            const unsigned comp = (unsigned)((lower >> 21) & 0x3u);
            // ⛔ G730 CORRECTNESS, not optimisation. This is the ONE compiled site that reads a VF
            // register as an INTEGER out of the state image, so a resident-and-dirty VF[is] would
            // make the image stale under it. `pextrw` at word index 2*comp is the LOW half of lane
            // `comp`, and `movsx r32, r16` completes the interpreter's own
            // `(int32_t)(int16_t)(value & 0xFFFF)`.
            const int bs = g730VfReg(is);
            if (bs >= 0)
            {
                e.pextrw(G610_RAX, (unsigned)bs, (uint8_t)(2u * comp));
                e.movsx16R(G610_RAX, G610_RAX);
            }
            else
            {
                e.movsx16M(G610_RAX, G610_RCX,
                           G610_OFF_VF + (int32_t)(16u * is) + (int32_t)(4u * comp));
            }
            e.storeViRaw(it);
            return true;
        }
        case G422_LK_MFIR:   // splat the sign-extended low 16 bits of vi[is] as RAW BITS
            if (s_g755ViChain)
                e.movsx16M(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
            else
            {
                e.movLd32(G610_RAX, G610_RCX, G610_OFF_VI + (int32_t)(4u * is));
                e.movsx16R(G610_RAX, G610_RAX);
            }
            e.movdToX(G610_X3, G610_RAX);
            e.shufps(G610_X3, G610_X3, 0x00u);
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            return true;

        // ---- quadword memory ----
        //
        // ⚠️ VU DATA RAM IS A DIFFERENT OBJECT FROM THE VU1State IMAGE. Quadword memory is addressed
        // through RDX and the state image through RCX, so no LQ/SQ can alias a resident VF or ACC
        // and residency needs no barrier here — only the VF SIDE of each transfer changes.
        case G422_LK_LQ:
            g610EmitAddrImm(e, is, imm11, dataSize);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            return true;
        case G422_LK_LQI:
            g610EmitAddrU16(e, is, dataSize);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            g610EmitViStep(e, is, +1);
            return true;
        case G422_LK_LQD:
            g610EmitViStep(e, is, -1);               // the decrement happens FIRST
            g610EmitAddrU16(e, is, dataSize, s_g755ViChain && is != 0u);
            e.movupsLdX(G610_X3, G610_RDX, G610_RAX, 0);
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            return true;
        case G422_LK_SQ:
        {
            g610EmitAddrImm(e, it, imm11, dataSize);
            const unsigned xs = g730SrcVf(e, is, G610_X3);
            g610EmitStoreQuadMasked(e, xs, dest);
            return true;
        }
        case G422_LK_SQI:
        {
            g610EmitAddrU16(e, it, dataSize);
            const unsigned xs = g730SrcVf(e, is, G610_X3);
            g610EmitStoreQuadMasked(e, xs, dest);
            g610EmitViStep(e, it, +1);
            return true;
        }
        case G422_LK_SQD:
        {
            g610EmitViStep(e, it, -1);
            g610EmitAddrU16(e, it, dataSize, s_g755ViChain && it != 0u);
            const unsigned xs = g730SrcVf(e, is, G610_X3);
            g610EmitStoreQuadMasked(e, xs, dest);
            return true;
        }

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
            e.m_vfInX3 = -1;                        // X3 now holds a VI scalar, not a full VF
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
        s_g747BoundValid = false;
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
        // ⭐ G730: choose the host-register residency for this whole block BEFORE any byte is
        // emitted, and install it for the duration. `G730Scope` is RAII because this function has
        // a dozen refusal returns and a leaked context would let the NEXT body name pool registers
        // its own prologue never loaded.
        G730Ctx g730Ctx;
        g730PlanBlock(g730Ctx, firstIndex, count);
        G730Scope g730Scope(&g730Ctx);
        g730EmitPrologue(e, g730Ctx);

        // Reserve the prologue; if `anyHist`, load the ring cursor once (G504's exactness argument).
        if (anyHist)
        {
            e.movLd32(G610_R10, G610_R9, G610_OFF_HHEAD);
            e.shlImm(G610_R10, 5); // G726: carry the cursor as a BYTE OFFSET, slot*32
        }
        e.movImm64(G610_R11, (uint64_t)(uintptr_t)&g610Consts);

        // ⭐ G744: a G610 block is straight-line with a single `ret`, so "pushes after j" is an
        // exact backward count and the whole block is one planned run. The scope clears the plan on
        // every exit path -- this function has a dozen refusal returns, and a leaked plan would let
        // the NEXT body elide by an index that means something else there.
        G744PlanScope g744Scope;
        g744PlanRun(fused, firstIndex, count, true);
        if (g744HistStatOn())
            s_g744HistPairs += count;

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
                g730EmitRepinVf0(e, G610_X4);
                e.movStImm32(G610_RCX, G610_OFF_VI, 0u);
            }
            if (e.bad)
                break;
        }
        if (anyHist && usesHist)
        {
            e.flushHistAdvance();
            e.shrImm(G610_R10, 5); // G726: byte offset -> slot index, the form `head` stores
            e.movSt32(G610_R10, G610_R9, G610_OFF_HHEAD);
        }
        g730EmitEpilogue(e, g730Ctx);
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
        g752DumpCode(e, "block");
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
            if (std::memcmp(ha.e[k].vec, hb.e[k].vec, 16) != 0)
            { *what = "hist.vec"; *at = k; return false; }
            if (ha.e[k].stamp != hb.e[k].stamp) { *what = "hist.stamp"; *at = k; return false; }
            if (ha.e[k].dest != hb.e[k].dest) { *what = "hist.dest"; *at = k; return false; }
            if (ha.e[k].kind != hb.e[k].kind) { *what = "hist.kind"; *at = k; return false; }
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
        // G744: its own flag, so the elision census can be read without arming [G610:jit] (and so
        // an inert arm cannot be mistaken for a clean control -- `arm=` is printed).
        // G745: `g744HistReport()` used to be called here, "so the elision census can be read
        // without arming [G610:jit]" — which it could not be, because every caller of this function
        // is already inside `if (g610StatOn())`. It is now called from the block counter's own
        // cadence in vu1_g490_block_run.inc, outside that guard, which is what the comment meant.
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
        // G730's translate-time residency census. It is the proof that the allocator actually
        // reached the emitter: an unarmed run prints `on=0 bound=0`, which a clean control is
        // otherwise indistinguishable from (Rule 69/90).
        g730Report();
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
