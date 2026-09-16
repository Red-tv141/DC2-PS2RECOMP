// ============================================================================================
// G730 — VU1 HOST-REGISTER RESIDENCY ALLOCATOR FOR THE G610/G612 COMPILED BACKENDS
// ============================================================================================
//
// WHAT THIS IS. G610 translates a VU1 pair into x86-64 with every operand address, dest mask and
// immediate baked in — but it reconstructs the VU machine state from the `VU1State` image on EVERY
// instruction. A single FMAC pair emits, unconditionally:
//
//     movups xmm0, [rcx + VF + 16*fs]      ; vs        — a LOAD
//     movups xmm1, [rcx + VF + 16*ft]      ; vt        — a LOAD
//     movups xmm2, [rcx + ACC]             ; acc       — a LOAD
//     ... the arithmetic ...
//     movups [r9 + r10 + 0/16/28]          ; G430 history (3 stores)
//     movups [rcx + VF + 16*fd], xmm2      ; dest      — a STORE
//
// Seven memory operations for one packed multiply-add. Two of them are worse than they look: the
// ACC store of pair `j` and the ACC load of pair `j+1` are the SAME address, so consecutive FMACs
// are chained through a **store-to-load forward** — ~5-6 cycles of pure latency sitting inside the
// dependency chain of every pair. `[G483:runprof] loopNsPerPair` reads 6.7-16.6 ns = 21-53 cycles
// against the ~5 cycles the packed SSE arithmetic itself costs.
//
// ⛔ G711'S BALLAST LADDER CANNOT SEE THIS AND SAYS SO. It priced ALL per-pair memory traffic at
// 2.349 ms/f by re-executing each class N extra times — and its own caveat is that "extra
// INDEPENDENT stores do not lengthen the ACC dependency chain", i.e. the ladder measures the
// THROUGHPUT cost of the traffic and not the LATENCY cost of the chain. The 2.349 ms/f is a lower
// bound on this lever, not a ceiling on it. (plans/phase-G726-plan.md priced the throughput terms
// individually and found ~0.1-0.3 ms/f reachable; that arithmetic is about the *stores*, and it is
// correct about the stores. This file attacks the *chain*.)
//
// WHAT IT DOES. Before a block or a region is emitted, its pairs are scanned and the hottest VU
// locations — VF[0..31] and ACC — are BOUND to host XMM registers for the whole body. A bound
// location is loaded ONCE in the prologue, lives in its register across every instruction, and is
// written back ONCE at the exit. Inside the body:
//
//   * an operand read of a bound location emits NO instruction at all (the value is already there);
//   * a full-dest write emits `movaps dst, src`, which Skylake eliminates at rename — zero latency,
//     zero execution port;
//   * a PARTIAL-dest write, which today is a load + blend + store round trip, becomes a pure
//     register blend (and `dest == 0x8` collapses to a single `movss dst, src`);
//   * the ACC chain of consecutive FMACs becomes register-to-register: `addps xmmACC, prod`.
//
// ⭐ WHY THE BINDING IS WHOLE-BODY AND FIXED, NOT A LINEAR SCAN. A region is a whole CFG with
// back-edges and multiple entry thunks and up to `G612_MAX_BB * 4` exit stubs, ALL of which funnel
// through one shared epilogue. A per-instruction allocator would need a merge/phi rule at every
// join and a per-exit flush set. A binding that is INVARIANT over the whole region has neither
// problem by construction: every basic block agrees on which register holds what, the entry thunks
// all emit the same prologue, and the one shared epilogue writes back one known set. The cost is
// that a location must be hot enough to earn a whole register for the body; the selection below is
// exactly that decision, and it is a PERFORMANCE decision — a mis-selection can never be wrong,
// only slower, because the emitter's `g730RegOf` is the single authority on what is bound.
//
// ⛔ THE REGISTERS ARE CALLEE-SAVED, AND THAT IS DELIBERATE. Windows x64 has six volatile XMM
// registers and G610 uses all six as scratch (vs, b, result, memory value, two blend temporaries).
// The residency pool is therefore xmm6..xmm15, which the ABI says this function must preserve — so
// a body with `K > 0` bindings gets a real prologue and epilogue:
//
//     sub  rsp, 8 + 16*K            (blocks; RSP is 8 mod 16 at entry, so this aligns it)
//     movaps [rsp + 16*i], xmm(6+i) ... save
//     movups xmm(6+i), [rcx + loc]  ... load
//     ...
//     movups [rcx + loc], xmm(6+i)  ... write back
//     movaps xmm(6+i), [rsp + 16*i] ... restore
//     add  rsp, 8 + 16*K
//
// That is 4 memory operations per bound register, ALL INDEPENDENT (they pipeline at 2 loads + 1
// store per cycle and sit on no dependency chain), against removing (uses - 1) chained ones. The
// `DC2_G730_MINUSE` threshold is what keeps the trade positive on the short bodies this port
// actually runs (`[G490:block] avg = 9.15` pairs; a region round trip averages 19.68).
//
// ⚠️ A REGION ALREADY HAD A PROLOGUE — G612 pushes RBP/RSI/RDI — so this is not a new hazard class
// there. It IS new for blocks. Neither form registers unwind data (`RtlAddFunctionTable`), exactly
// as G612's three pushes never did: an emitted body calls nothing and cannot fault under any input,
// so the only consequence is that a crash-handler stack walk cannot see through the frame. Same
// posture as the shipped region backend, documented rather than changed.
//
// ⛔ THE FOUR PLACES A RESIDENT VALUE COULD GO STALE, each handled at source:
//   1. `MTIR` reads a VF lane as a 16-bit INTEGER out of the state image. With VF[is] resident and
//      dirty that read is stale, so the emitter extracts the lane with `pextrw` + `movsx` instead.
//      This is the one site where residency is a CORRECTNESS obligation and not an optimisation.
//   2. The G425 VF0 re-pin writes VF[0] in the image; if VF[0] is bound it must write the register.
//   3. Every region exit — budget, dynamic jump, unemittable pair, E-bit — reaches the SHARED
//      epilogue, which is where the write-back lives. There is no other way out of a region body.
//   4. `st.q`, `st.p` and `st.i` are NEVER bound: Q is published by the hook between the two halves
//      of a G611 Q-split, and I is written by the G652 LOI path directly into the image.
//
// ⭐ v1 WRITES BACK EVERY BOUND LOCATION, whether or not the body wrote it. Writing back a value
// that was loaded and never modified is semantically a no-op, so correctness does not depend on the
// use-scan being exact — and the use-scan is a heuristic. The `written` refinement is a separate,
// separately-verified step.
//
// VERIFY. `DC2_G610_VERIFY=1` / `DC2_G612_VERIFY=1` compare the ENTIRE VU1State (VF, VI, ACC, Q, P,
// I, pc, mac, clip, status), all eight G430 history entries and all of VU data RAM after every
// compiled block and region against the shipped interpreter re-run from the identical pre-state. A
// missed write-back, a stale operand or a wrong blend is caught on its FIRST occurrence.
//
//   DC2_G730_RA=1        arm the allocator (default OFF at bring-up)
//   DC2_G730_NO_RA=1     rollback once promoted
//   DC2_G730_MINUSE=<n>  minimum weighted uses to earn a register (default 3)
//   DC2_G730_MAXREG=<n>  cap on bound registers, 0..10 (default 10)
//   DC2_G730_STAT=1      the `[G730:ra]` translate-time census
//
// ⛔ THE GATE IS `[G483:runprof] loopNsPerPair` AT MATCHED `pairs/call` — never frame time.
// `dragon` frame time has a 2.7-3.2 ms/f arm-independent spread (plans/phase-G726-plan.md), and
// this is a per-pair lever. Convert with `dVU1busy = dLoopNsPerPair * pairs/f`.
// ============================================================================================

// -------------------------------------------------------------------------------------------
// §1 LOCATIONS AND THE POOL
// -------------------------------------------------------------------------------------------
constexpr unsigned G730_LOC_ACC = 32u; // 0..31 are VF[r]
constexpr unsigned G730_NLOC = 33u;
constexpr unsigned G730_POOL_BASE = 6u; // xmm6
constexpr unsigned G730_NPOOL = 10u;    // xmm6..xmm15

// ⭐ THE COST MODEL'S TWO WEIGHTS (the argument for them is in §3). A DEF-USE EDGE costs a
// store-to-load forward, ~5-6 cycles of pure dependency latency, and that is what a register
// removes. A plain READ costs a pipelined load, which on this machine is nearly free at 2/cycle and
// sits on no chain. They are declared here rather than beside the scan because §2's binding
// thresholds are expressed in units of `G730_W_CHAIN` — "at least N chain edges".
constexpr uint32_t G730_W_CHAIN = 6u;
constexpr uint32_t G730_W_USE = 1u;
// A partial dest is a READ-MODIFY-WRITE of its own destination, so it carries a forward even when
// nothing else ever reads the location.
constexpr uint32_t G730_W_RMW = 6u;

static_assert(G730_POOL_BASE + G730_NPOOL == 16u, "G730: the pool is exactly xmm6..xmm15");

// The state-image displacement a bound location writes back to.
inline int32_t g730LocDisp(unsigned loc)
{
    return (loc == G730_LOC_ACC) ? G610_OFF_ACC : (G610_OFF_VF + (int32_t)(16u * loc));
}

struct G730Ctx
{
    bool active = false;
    int8_t reg[G730_NLOC];    // host xmm index, or -1 when the location stays in memory
    uint8_t loc[G730_NPOOL];  // pool slot -> location id
    uint8_t nBound = 0u;
    int32_t frameBytes = 0;   // the `sub rsp` amount this body's prologue emits

    // ⭐⭐ THE DIRTY SET, AND WHY IT IS RECORDED BY THE EMITTER AND NOT BY THE SCAN.
    //
    // A bound location that the body only ever READS does not need writing back: the epilogue would
    // store the same bits the prologue loaded. Eliding that store removes 1 of the 4 memory
    // operations a bound register costs per body entry — 25% of the prologue, which is the term
    // §8's thresholds are fighting.
    //
    // ⛔ BUT "never written" is a CORRECTNESS claim, and the use scan is a HEURISTIC. If the scan
    // missed a write the epilogue would drop a real result and the state would silently diverge.
    // So this bit is set by `g730StoreLoc` and `g730EmitRegPin` — the emitter functions that
    // actually emit the write — never by the planner. Every path that writes a pool register goes
    // through one of them, so the set cannot under-approximate: an unrecorded write is impossible
    // rather than unlikely. The scan keeps its "wrong only means slower" property.
    uint16_t dirty = 0u;      // bit k = pool slot k was written by emitted code
    static_assert(G730_NPOOL <= 16u, "G730: the dirty mask is 16 bits");

    G730Ctx()
    {
        for (unsigned k = 0u; k < G730_NLOC; ++k)
            reg[k] = -1;
        for (unsigned k = 0u; k < G730_NPOOL; ++k)
            loc[k] = 0u;
    }
    void markDirtyReg(unsigned xmm)
    {
        if (xmm >= G730_POOL_BASE && xmm < G730_POOL_BASE + G730_NPOOL)
            dirty |= (uint16_t)(1u << (xmm - G730_POOL_BASE));
    }
};

// THE ONE AUTHORITY. Every emitter site asks this, and only this, whether a location is resident.
// A null or inactive context is the pre-G730 emitter, byte for byte.
thread_local G730Ctx *s_g730Cur = nullptr;

// RAII, because `g610TranslateBlock` and `g612EmitRegion` both have a dozen early returns and a
// leaked context would let the NEXT body emit register references its own prologue never made.
struct G730Scope
{
    G730Ctx *prev;
    explicit G730Scope(G730Ctx *c) : prev(s_g730Cur) { s_g730Cur = c; }
    ~G730Scope() { s_g730Cur = prev; }
};

inline int g730RegOf(unsigned loc)
{
    const G730Ctx *const c = s_g730Cur;
    return (c != nullptr && c->active) ? c->reg[loc] : -1;
}
inline int g730VfReg(unsigned r) { return g730RegOf(r); }
inline int g730AccReg() { return g730RegOf(G730_LOC_ACC); }

// -------------------------------------------------------------------------------------------
// §2 ARMS AND TUNING. All process-scope, all read once, all consumed at TRANSLATE time — never
// inside an emitted body and never per executed pair (Rule 13).
// -------------------------------------------------------------------------------------------
#ifndef DC2_G730_DEFAULT_ON
#define DC2_G730_DEFAULT_ON 0
#endif

inline uint32_t g730EnvU32(const char *name, uint32_t dflt)
{
    const char *const s = std::getenv(name);
    if (s == nullptr || *s == '\0')
        return dflt;
    return (uint32_t)std::strtoul(s, nullptr, 0);
}

bool g730RaOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G730_NO_RA") &&
                             (DC2_G730_DEFAULT_ON != 0 || f50_12_vu_env_flag("DC2_G730_RA"));
    return s_on;
}
// ⭐ Blocks and regions are separately armable, because they are separately PROFITABLE. A block
// averages 9.15 pairs (`[G490:block] avg`) and pays its prologue once over that; a region round
// trip averages 19.68 pairs and its prologue amortises over whole loops. Measuring them together
// would leave a null unattributable, so each has its own rollback.
bool g730BlockOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G730_NO_BLOCK");
    return s_on;
}
bool g730RegionOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G730_NO_REGION");
    return s_on;
}
// Write back only the pool registers emitted code actually wrote. Rollback restores the v1
// behaviour (write back everything), which is what makes a null on this refinement attributable.
bool g730DirtyOn()
{
    static const bool s_on = !f50_12_vu_env_flag("DC2_G730_NO_DIRTY");
    return s_on;
}
// ⭐ v2: SEPARATE THRESHOLDS, because the two backends amortise their prologue over very different
// bodies (8.43 pairs per block entry vs 38.92 per region entry — measured, §4). One number cannot be
// right for both. `DC2_G730_MINUSE` still sets BOTH, so a single-knob sweep arm keeps working.
uint32_t g730MinUse()
{
    static const uint32_t s_n = g730EnvU32("DC2_G730_MINUSE", 0u);
    return s_n;
}
uint32_t g730MinUseBlock()
{
    // A block entry costs 4 ops per register over 8.43 pairs. Demand three chain edges.
    static const uint32_t s_n = [] {
        const uint32_t all = g730MinUse();
        return all ? all : g730EnvU32("DC2_G730_MINUSE_BLK", 3u * G730_W_CHAIN);
    }();
    return s_n;
}
uint32_t g730MinUseRegion()
{
    // A region entry amortises over 4.6x more pairs, so one chain edge in a loop body already pays.
    static const uint32_t s_n = [] {
        const uint32_t all = g730MinUse();
        return all ? all : g730EnvU32("DC2_G730_MINUSE_REG", 2u * G730_W_CHAIN);
    }();
    return s_n;
}
uint32_t g730MaxReg()
{
    static const uint32_t s_n = [] {
        const uint32_t v = g730EnvU32("DC2_G730_MAXREG", G730_NPOOL);
        return (v > G730_NPOOL) ? G730_NPOOL : v;
    }();
    return s_n;
}
const bool s_g730StatOn = f50_12_vu_env_flag("DC2_G730_STAT");
inline bool g730StatOn() { return s_g730StatOn; }

// Translate-time census. Cold: one update per compiled body, never per executed pair.
thread_local uint64_t s_g730Bodies = 0u, s_g730BodiesBound = 0u, s_g730BoundRegs = 0u;
thread_local uint64_t s_g730ScanPairs = 0u, s_g730MemSaved = 0u, s_g730MemAdded = 0u;
thread_local uint64_t s_g730Hist[G730_NPOOL + 1u] = {};
// ⭐ v2. `chains` is the number of def-use edges the scan found — the population this lever exists
// to remove — and `accBound` / `vfBound` say WHICH locations actually won a register. v1 reported
// only how MANY registers were bound, which cannot distinguish "ACC is resident on every FMAC body"
// from "ten rarely-written VF registers are resident and every chain is still in memory". That
// distinction is the entire difference between the v1 and v2 cost models.
thread_local uint64_t s_g730Chains = 0u, s_g730AccBound = 0u, s_g730AccBodies = 0u;
thread_local uint64_t s_g730ChainScore = 0u, s_g730BoundScore = 0u;
thread_local uint64_t s_g730WbEmitted = 0u, s_g730WbElided = 0u;

// -------------------------------------------------------------------------------------------
// §3 THE COST MODEL — ⭐⭐⭐ v2 SCORES DEF-USE CHAINS, NOT USE FREQUENCY.
//
// ⛔ WHY v1 WAS WRONG, AND THE MEASUREMENT THAT SAYS SO. v1 ranked a location by how OFTEN the body
// touched it. That model is wrong about this machine, and the G730 execution-level ladder is what
// proved it:
//
//     ctl (both backends)                 7.159 ns/pair   (mean of two controls)
//     noboth (L1+L2 -> generic executor) 12.562           => g - c = 6.14 ns/pair
//     pairs/call invariant to 0.02% across all five arms
//
// Solving with the 87.94% compiled share puts a COMPILED pair at ~13-21 cycles against ~5 for the
// packed SSE it contains. But G711's ballast ladder prices ALL per-pair memory traffic at
// 0.8125 ns = ~2.6 cycles. **The gap is not the number of memory operations. It is their LATENCY**,
// and the shape is exactly:
//
//     pair j    : ... mulps (4) -> addps (4) -> movups [rcx+VF+16*fd]
//     pair j+1  :     movups xmm0, [rcx+VF+16*fs]   <- STORE-TO-LOAD FORWARD, ~5-6 cycles
//
// 4 + 4 + 5 = 13-14 cycles, which is the measured figure.
//
// ⭐ THE CONSEQUENCE FOR SELECTION. A location that is READ ten times and never written costs almost
// nothing to leave in memory: ten independent loads pipeline at 2/cycle and sit on no chain.
// A location that is WRITTEN by one pair and READ by a later one costs ~5-6 cycles of pure
// dependency latency EVERY TIME, and that is precisely what a register removes. Ranking by
// frequency spends the ten callee-saved registers on the loads and leaves the chains in memory —
// which is why v1 measured -0.0712 ns/pair (-0.99%) with a perfect 0.01% workload identity.
//
// v2 therefore scores a **def-use edge**: every (write at pair j, read at pair k>j) inside the body.
// Frequency survives only as a small tie-breaker, because removing a load is still worth something.
//
// `w` is the block weight — 1 for a straight-line block, higher for a region basic block the CFG
// shows inside a loop, because a loop body executes many times per region entry.
//
// This is a HEURISTIC INPUT, not a correctness input: the emitter asks `g730RegOf`, never the scan,
// so a mis-scored location is only ever slower.
// -------------------------------------------------------------------------------------------

// One pair's VU-location reads and writes, in the order the emitter performs them.
struct G730PairIo
{
    uint8_t rd[4];
    uint8_t wr[3];
    uint8_t wrPartial[3];   // this WRITE is a read-modify-write blend, not a full overwrite
    uint8_t nRd = 0u;
    uint8_t nWr = 0u;
    void addRd(unsigned l) { if (nRd < 4u) rd[nRd++] = (uint8_t)l; }
    void addWr(unsigned l, bool partial)
    {
        if (nWr >= 3u)
            return;
        wr[nWr] = (uint8_t)l;
        wrPartial[nWr] = partial ? 1u : 0u;
        ++nWr;
    }
};

struct G730Use
{
    uint32_t n[G730_NLOC];
    int32_t lastWrite[G730_NLOC];   // pair index of the last writer, -1 = none in this walk
    G730Use()
    {
        for (unsigned k = 0u; k < G730_NLOC; ++k)
        {
            n[k] = 0u;
            lastWrite[k] = -1;
        }
    }
    // Start a new straight-line walk. Def-use edges are only counted WITHIN one walk, so a chain is
    // never claimed across a control-flow join the emitted code does not actually have.
    void newWalk()
    {
        for (unsigned k = 0u; k < G730_NLOC; ++k)
            lastWrite[k] = -1;
    }
};

// Decode one pair into the locations the emitter reads and writes. Mirrors `g610EmitUpper` /
// `g610EmitLower` opcode for opcode.
void g730PairIo(uint32_t upper, uint8_t upperKind, uint32_t lower, uint8_t lowerKind,
                uint8_t g425Flags, G730PairIo &io)
{
    // ---- upper ----
    if (upperKind != 0x6fu)
    {
        const G421UpperDesc d = g421DescTableConst.e[upperKind];
        if (d.cls != 0u)
        {
            const unsigned fs = FS(upper), ft = FT(upper), fd = FD(upper);
            const uint8_t dest = DEST(upper);
            const bool partial = (dest != 0xFu && dest != 0u);

            io.addRd(fs);                          // `vs` is read by every non-VNOP upper

            if (d.cls == 8u)                       // CLIPw: reads vt, writes only st.clip
            {
                io.addRd(ft);
            }
            else if (d.cls >= 3u && d.cls <= 7u)   // ITOF/FTOI/ABS/MOVE/MR32 -> VF[ft]
            {
                io.addWr(ft, partial);
            }
            else                                   // cls 1 -> VF[fd], cls 2 -> ACC
            {
                if (d.src == 0u || d.src == 3u)
                    io.addRd(ft);                  // broadcast lane / full vt
                if (d.op == 2u || d.op == 3u || d.op == 7u || d.op == 8u)
                    io.addRd(G730_LOC_ACC);        // MADD/MSUB/OPMSUB/OPMULA read ACC
                io.addWr((d.cls == 1u) ? fd : G730_LOC_ACC, partial);
            }
        }
    }

    // ---- lower ----
    {
        const unsigned it = LIT(lower), is = LIS(lower);
        const uint8_t dest = (uint8_t)((lower >> 21) & 0xFu);
        const bool partial = (dest != 0xFu && dest != 0u);
        switch (lowerKind)
        {
        case G422_LK_MOVE:
        case G422_LK_MR32:
            io.addRd(is);
            io.addWr(it, partial);
            break;
        case G422_LK_MTIR:
            io.addRd(is);
            break;
        case G422_LK_MFIR:
        case G422_LK_LQ:
        case G422_LK_LQI:
        case G422_LK_LQD:
            io.addWr(it, partial);
            break;
        case G422_LK_SQ:
        case G422_LK_SQI:
        case G422_LK_SQD:
            io.addRd(is);
            break;
        default:
            break;
        }
    }

    // ---- the G425 VF0 re-pin ---- (a full overwrite, never a blend)
    if ((g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
        io.addWr(0u, false);
}

// Score one pair at index `j` of the current straight-line walk.
void g730ScanPair(uint32_t upper, uint8_t upperKind, uint32_t lower, uint8_t lowerKind,
                  uint8_t g425Flags, uint32_t w, int32_t j, G730Use &u)
{
    ++s_g730ScanPairs;
    G730PairIo io;
    g730PairIo(upper, upperKind, lower, lowerKind, g425Flags, io);

    // READS FIRST — a pair's reads observe the value a PREVIOUS writer left, never its own write.
    for (unsigned k = 0u; k < io.nRd; ++k)
    {
        const unsigned l = io.rd[k];
        u.n[l] += w * G730_W_USE;
        if (u.lastWrite[l] >= 0)
        {
            u.n[l] += w * G730_W_CHAIN;            // ⭐ the store-to-load forward this removes
            ++s_g730Chains;
        }
    }
    for (unsigned k = 0u; k < io.nWr; ++k)
    {
        const unsigned l = io.wr[k];
        u.n[l] += w * G730_W_USE;
        // A partial dest re-reads its own destination to blend it: a forward of its own.
        if (io.wrPartial[k] != 0u && u.lastWrite[l] >= 0)
        {
            u.n[l] += w * G730_W_RMW;
            ++s_g730Chains;
        }
        u.lastWrite[l] = j;
    }
}

// -------------------------------------------------------------------------------------------
// §4 SELECTION.
//
// Bind the highest-scoring locations, in descending order, subject to `MINUSE` and `MAXREG`.
//
// ⭐ v2: THE THRESHOLD IS NOW MEASURED AGAINST THE PROLOGUE, WHICH IS PER BODY ENTRY. A bound
// register costs 4 memory operations at EVERY entry (save, load, write-back, restore). The ladder
// measured what a body entry actually amortises over on `dragon`:
//
//     [G610:jit] avg = 8.43 pairs per BLOCK entry
//     [G612:reg] avg = 38.92 pairs per REGION entry
//
// So a 5-register binding adds 20 memory ops over 8.43 pairs in a block — **2.37 ops/pair** — and
// over 38.92 pairs in a region — **0.51 ops/pair**. Against ~7 memory ops in a pair, the block form
// has to remove a third of them just to break even, and the region form only a fourteenth. That is
// why the two backends have separate rollbacks, and why `MINUSE` defaults differently for each: a
// score is only worth a register if it clears the prologue it forces.
//
// `G730_W_CHAIN = 6` means one def-use edge scores 6, so the default `MINUSE` of 12 is "at least two
// chain edges, or one chain edge in a loop body". Frequency alone (`G730_W_USE = 1`) can no longer
// buy a register on its own — which is exactly the v1 defect the ladder exposed.
// -------------------------------------------------------------------------------------------
void g730Select(G730Ctx &ctx, const G730Use &u, bool regionForm)
{
    ctx.active = false;
    ctx.nBound = 0u;
    ctx.frameBytes = 0;
    for (unsigned k = 0u; k < G730_NLOC; ++k)
        ctx.reg[k] = -1;

    if (!g730RaOn())
        return;
    if (regionForm ? !g730RegionOn() : !g730BlockOn())
        return;

    const uint32_t minUse = regionForm ? g730MinUseRegion() : g730MinUseBlock();
    const uint32_t maxReg = g730MaxReg();
    if (maxReg == 0u)
        return;

    // Selection sort over 33 candidates — this runs once per COMPILED BODY, in the cold rebuild
    // pass, against a code cache that translates each live microprogram image once.
    bool taken[G730_NLOC] = {};
    for (unsigned slot = 0u; slot < maxReg; ++slot)
    {
        unsigned best = G730_NLOC;
        uint32_t bestN = 0u;
        for (unsigned k = 0u; k < G730_NLOC; ++k)
        {
            if (taken[k] || u.n[k] < minUse)
                continue;
            if (u.n[k] > bestN)
            {
                bestN = u.n[k];
                best = k;
            }
        }
        if (best == G730_NLOC)
            break;
        taken[best] = true;
        ctx.loc[slot] = (uint8_t)best;
        ctx.reg[best] = (int8_t)(G730_POOL_BASE + slot);
        ++ctx.nBound;
        s_g730MemSaved += (uint64_t)(bestN >= 1u ? bestN - 1u : 0u);
        s_g730BoundScore += bestN;
        if (best == G730_LOC_ACC)
            ++s_g730AccBound;
    }
    // The whole score present in this body, bound or not — so `boundScore / chainScore` is the
    // share of the def-use population the ten registers actually captured. That fraction is what
    // says whether the CAP is binding or the THRESHOLD is.
    for (unsigned k = 0u; k < G730_NLOC; ++k)
        s_g730ChainScore += u.n[k];
    if (u.n[G730_LOC_ACC] != 0u)
        ++s_g730AccBodies;

    if (ctx.nBound == 0u)
        return;
    ctx.active = true;
    // Blocks enter with RSP ≡ 8 (mod 16); regions have already pushed three GPRs, so theirs is
    // ≡ 0. Both must land 16-byte aligned for `movaps`.
    ctx.frameBytes = (int32_t)(16u * ctx.nBound) + (regionForm ? 0 : 8);

    ++s_g730BodiesBound;
    s_g730BoundRegs += ctx.nBound;
    s_g730MemAdded += (uint64_t)ctx.nBound * 4ull;
}

// -------------------------------------------------------------------------------------------
// §5 PROLOGUE / EPILOGUE
// -------------------------------------------------------------------------------------------
void g730EmitPrologue(G610Emit &e, const G730Ctx &ctx)
{
    if (!ctx.active)
        return;
    e.subImm64Rsp(ctx.frameBytes);
    for (unsigned k = 0u; k < ctx.nBound; ++k)
        e.movapsStRsp(G730_POOL_BASE + k, (int32_t)(16u * k));
    for (unsigned k = 0u; k < ctx.nBound; ++k)
        e.movupsLd(G730_POOL_BASE + k, G610_RCX, g730LocDisp(ctx.loc[k]));
}

void g730EmitEpilogue(G610Emit &e, const G730Ctx &ctx)
{
    if (!ctx.active)
        return;
    // ⭐ Write back only the pool registers EMITTED CODE actually wrote. A read-only binding would
    // store back exactly the bits the prologue loaded, so eliding it is a no-op by construction —
    // and `ctx.dirty` is maintained by the emitter itself (see G730Ctx), never by the use scan, so
    // it can never under-approximate the real write set.
    //
    // ⚠️ The SAVE and RESTORE are NOT elidable the same way: the Windows x64 ABI makes xmm6..xmm15
    // callee-saved whether this body writes them or not, and the prologue's LOAD is needed even for
    // a write-only binding because a partial dest blends against the old value.
    const bool elide = g730DirtyOn();
    for (unsigned k = 0u; k < ctx.nBound; ++k)
    {
        if (elide && (ctx.dirty & (uint16_t)(1u << k)) == 0u)
        {
            ++s_g730WbElided;
            continue;
        }
        ++s_g730WbEmitted;
        e.movupsSt(G730_POOL_BASE + k, G610_RCX, g730LocDisp(ctx.loc[k]));
    }
    for (unsigned k = 0u; k < ctx.nBound; ++k)
        e.movapsLdRsp(G730_POOL_BASE + k, (int32_t)(16u * k));
    e.addImm64Rsp(ctx.frameBytes);
}

// -------------------------------------------------------------------------------------------
// §6 THE OPERAND AND DESTINATION ACCESSORS. Every VF/ACC touch in the two emitters goes through
// exactly one of these four, which is what makes the stale-value audit in the header finite.
// -------------------------------------------------------------------------------------------

// Return a register holding VF[r] for READ-ONLY use, loading into `scratch` when it is not bound.
// ⚠️ The returned register may be a POOL register; the caller must not write it.
inline int g747ForwardedVfReg(const G610Emit &e, unsigned r)
{
    if (e.m_vfInX2 == (int)r) return G610_X2;
    if (e.m_vfInX3 == (int)r) return G610_X3;
    return -1;
}

inline unsigned g730SrcVf(G610Emit &e, unsigned r, unsigned scratch)
{
    const int b = g730VfReg(r);
    if (b >= 0)
        return (unsigned)b;
    const int forwarded = g747ForwardedVfReg(e, r);
    if (forwarded >= 0)
    {
        // Copy into the caller's scratch: later arithmetic may overwrite X2 before using vs.
        if (scratch != (unsigned)forwarded) e.movaps(scratch, (unsigned)forwarded);
        ++s_g747VfForwards;
    }
    else
        e.movupsLd(scratch, G610_RCX, G610_OFF_VF + (int32_t)(16u * r));
    if (scratch == G610_X3)
        e.m_vfInX3 = g747VfForwardOn() && r != 0u ? (int)r : -1;
    return scratch;
}

// Materialise ACC into `scratch` for a caller that is going to MODIFY it (MSUB's minuend, OPMSUB's
// minuend, MADD when the accumulator is the destination operand).
inline void g730LoadAccInto(G610Emit &e, unsigned scratch)
{
    const int b = g730AccReg();
    if (b >= 0)
    {
        e.movaps(scratch, (unsigned)b);
        return;
    }
    // ⭐ G746 PART 5: the previous pair left the architectural ACC in X2 and nothing since has
    // written it. Either the caller already wants it there (no instruction at all) or a register
    // move replaces the load.
    if (e.m_accInX2)
    {
        if (scratch != G610_X2)
            e.movaps(scratch, G610_X2);
        return;
    }
    e.movupsLd(scratch, G610_RCX, G610_OFF_ACC);
}

// `xd = xd + ACC` — the MADD form where the PRODUCT is the destination operand (G611's measured
// operand order), so ACC is a read-only source and can stay where it is.
inline void g730AddAccInto(G610Emit &e, unsigned xd)
{
    const int b = g730AccReg();
    if (b >= 0)
        e.addps(xd, (unsigned)b);
    else if (e.m_accInX2 && xd != G610_X2)
        e.addps(xd, G610_X2);                     // ⭐ G746 PART 5
    else
        e.addpsM(xd, G610_RCX, G610_OFF_ACC);
}

// ⭐ G746: the same sum written non-destructively — `xd = xs1 + ACC`, with `xs1` (the product) as
// the FIRST source so the operand order G611 measured is preserved exactly. On AVX this is one
// instruction whether ACC is resident or not; on SSE it is the `movaps xd, xs1` the call site used
// to spell, followed by the identical add.
inline void g730AddAccInto3(G610Emit &e, unsigned xd, unsigned xs1)
{
    const int b = g730AccReg();
    if (b >= 0)
        e.addps3(xd, xs1, (unsigned)b);
    else if (e.m_accInX2)
        e.addps3(xd, xs1, G610_X2);               // ⭐ G746 PART 5 — `vaddps X2, X3, X2`
    else
        e.addps3M(xd, xs1, G610_RCX, G610_OFF_ACC);
}

// Write `xv` into location `loc` under `dest`, in a register when the location is bound and through
// `G610Emit::storeDest` when it is not. `xt1`/`xt2` are clobbered; `xv` is preserved.
//
// ⭐ The register forms are why this lever is larger than the store count suggests: the memory form
// of a PARTIAL dest is a load, five ALU ops and a store, and the load forwards from whatever store
// last wrote that VF register. The register form is five ALU ops and no memory at all.
void g730StoreLoc(G610Emit &e, unsigned xv, unsigned loc, uint8_t dest,
                  unsigned xt1, unsigned xt2)
{
    // Covers upper AND lower writes, including partial LQ/MOVE/MFIR writes to a cached VF.
    // Full upper writes establish the new X2 identity only after this store has been emitted.
    if (dest != 0u && e.m_vfInX2 == (int)loc)
        e.m_vfInX2 = -1;
    if (dest != 0u && e.m_vfInX3 == (int)loc)
        e.m_vfInX3 = -1;
    // LQ/LQI/LQD/MFIR/MR32 produce X3 even when their destination mask is partial.
    // Only a full write establishes a VF identity; partial writes invalidate that scratch.
    if (xv == G610_X3)
        e.m_vfInX3 = (g747VfForwardOn() || s_g755LowerForward) && dest == 0xFu && loc > 0u && loc < 32u
                        ? (int)loc : -1;
    const int b = g730RegOf(loc);
    if (b < 0)
    {
        if (s_g753PartialVf && loc > 0u && loc < 32u && dest != 0u && dest != 0xFu &&
            (xv == G610_X2 || xv == G610_X3))
        {
            // Arithmetic history has already consumed the raw result. Keep the merged VF
            // value in its result scratch so the next consumer bypasses the store/load edge.
            // Only partial producers establish this identity; full-register allocation remains off.
            e.movupsLd(xt1, G610_RCX, g730LocDisp(loc));
            e.blendpsR(xt1, xv, kG746BlendImm[dest]);
            e.movupsSt(xt1, G610_RCX, g730LocDisp(loc));
            e.movaps(xv, xt1);
            if (xv == G610_X2) e.m_vfInX2 = (int)loc;
            else e.m_vfInX3 = (int)loc;
            return;
        }
        e.storeDest(xv, G610_RCX, g730LocDisp(loc), dest, xt1, xt2);
        return;
    }
    const unsigned dr = (unsigned)b;
    if (dest == 0u)
        return;                                   // `applyDest` writes nothing — and no dirty bit
    // Past this point emitted code WILL write the pool register, so record it before any early
    // return. This function and `g730EmitRegPin` are the only two writers of a pool register, which
    // is what makes `ctx.dirty` a sound over-approximation of the real write set.
    s_g730Cur->markDirtyReg(dr);
    if (dest == 0xFu)
    {
        if (dr != xv)
            e.movaps(dr, xv);                     // eliminated at rename on this host
        return;
    }
    if (dest == 0x8u)
    {
        // Lane 0 only. `movss xmm_d, xmm_s` merges the low 32 bits and leaves the upper 96 — which
        // is exactly `applyDest` with dest == 0x8, in one uop.
        e.movssR(dr, xv);
        return;
    }
    // ⭐ G746: with both operands in registers the select is ONE `blendps` — the pool register is
    // the destination and survives in its non-`dest` lanes by definition of the instruction. Six
    // instructions and one constant-pool load removed; `xv` and both scratches are untouched.
    if (g746BlendOn())
    {
        if (g746DestStatOn())
            ++s_g746DestBlendReg;
        e.blendpsR(dr, xv, kG746BlendImm[dest & 0xFu]);
        return;
    }
    // The same bit-select the interpreter performs, with both operands already in registers.
    e.movapsLd(xt1, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
    e.andnps(xt1, dr);                            // xt1 = ~mask & old
    e.movaps(xt2, xv);
    e.andpsM(xt2, G610_R11, G610_C_DEST + (int32_t)(16u * dest));
    e.orps(xt2, xt1);
    e.movaps(dr, xt2);
}

inline void g730StoreVf(G610Emit &e, unsigned xv, unsigned r, uint8_t dest,
                        unsigned xt1, unsigned xt2)
{
    g730StoreLoc(e, xv, r, dest, xt1, xt2);
}
inline void g730StoreAcc(G610Emit &e, unsigned xv, uint8_t dest, unsigned xt1, unsigned xt2)
{
    g730StoreLoc(e, xv, G730_LOC_ACC, dest, xt1, xt2);
}

// The G425 re-pin of VF0 = (0,0,0,1). VI0 is an integer slot and is unaffected by residency.
// The SECOND of the two writers of a pool register (see G730Ctx's dirty-set note).
inline void g730EmitRepinVf0(G610Emit &e, unsigned scratch)
{
    const int b = g730VfReg(0u);
    if (b >= 0)
    {
        s_g730Cur->markDirtyReg((unsigned)b);
        e.movapsLd((unsigned)b, G610_R11, G610_C_VF0);
        return;
    }
    e.movapsLd(scratch, G610_R11, G610_C_VF0);
    e.movupsSt(scratch, G610_RCX, G610_OFF_VF);
}

// -------------------------------------------------------------------------------------------
// §7 REPORTING
// -------------------------------------------------------------------------------------------
void g730Report()
{
    if (!g730StatOn())
        return;
    std::fprintf(stderr,
                 "[G730:ra] on=%d blk=%d reg=%d minUseBlk=%u minUseReg=%u maxReg=%u | "
                 "bodies=%llu bound=%llu regs=%llu avgRegs=%.2f | scanPairs=%llu "
                 "memSaved=%llu memAdded=%llu\n",
                 g730RaOn() ? 1 : 0, g730BlockOn() ? 1 : 0, g730RegionOn() ? 1 : 0,
                 g730MinUseBlock(), g730MinUseRegion(), g730MaxReg(),
                 (unsigned long long)s_g730Bodies, (unsigned long long)s_g730BodiesBound,
                 (unsigned long long)s_g730BoundRegs,
                 s_g730BodiesBound ? (double)s_g730BoundRegs / (double)s_g730BodiesBound : 0.0,
                 (unsigned long long)s_g730ScanPairs, (unsigned long long)s_g730MemSaved,
                 (unsigned long long)s_g730MemAdded);
    // ⭐ v2's OWN NUMBERS. `chains` is the def-use population the lever exists to remove;
    // `capture` is the share of the total score the ten registers actually took (a low capture with
    // a high chain count means the CAP binds, a high capture with a small win means the MODEL does);
    // `accBound/accBodies` is the single most diagnostic line, because the ACC chain is the one
    // this phase predicted would dominate.
    std::fprintf(stderr,
                 "[G730:chain] chains=%llu score=%llu boundScore=%llu capture=%.1f%% | "
                 "accBound=%llu / accBodies=%llu (%.1f%%)\n",
                 (unsigned long long)s_g730Chains, (unsigned long long)s_g730ChainScore,
                 (unsigned long long)s_g730BoundScore,
                 s_g730ChainScore ? 100.0 * (double)s_g730BoundScore / (double)s_g730ChainScore : 0.0,
                 (unsigned long long)s_g730AccBound, (unsigned long long)s_g730AccBodies,
                 s_g730AccBodies ? 100.0 * (double)s_g730AccBound / (double)s_g730AccBodies : 0.0);
    std::fprintf(stderr,
                 "[G730:wb] dirtyElide=%d emitted=%llu elided=%llu (%.1f%% of write-backs gone)\n",
                 g730DirtyOn() ? 1 : 0, (unsigned long long)s_g730WbEmitted,
                 (unsigned long long)s_g730WbElided,
                 (s_g730WbEmitted + s_g730WbElided)
                     ? 100.0 * (double)s_g730WbElided /
                           (double)(s_g730WbEmitted + s_g730WbElided)
                     : 0.0);
    std::fprintf(stderr, "[G730:hist]");
    for (unsigned k = 0u; k <= G730_NPOOL; ++k)
        std::fprintf(stderr, " %u=%llu", k, (unsigned long long)s_g730Hist[k]);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

inline void g730NoteBody(const G730Ctx &ctx)
{
    ++s_g730Bodies;
    s_g730Hist[ctx.nBound <= G730_NPOOL ? ctx.nBound : G730_NPOOL] += 1u;
}

// -------------------------------------------------------------------------------------------
// §8 THE BLOCK PLANNER. (The REGION planner lives in vu1_g612_native_region.inc, because it needs
// that file's CFG plan to find the loop bodies; it calls the same `g730ScanPair`/`g730Select`.)
// -------------------------------------------------------------------------------------------
void g730PlanBlock(G730Ctx &ctx, uint32_t firstIndex, uint32_t count)
{
    G730Use u;
    // A G610 block is ONE straight-line run by construction, so the whole body is one walk and
    // every def-use edge inside it is real.
    u.newWalk();
    const G479FusedPair *const fused = s_g410Cache.fused.data();
    for (uint32_t j = 0u; j < count; ++j)
    {
        const G479FusedPair &fp = fused[firstIndex + j];
        g730ScanPair((uint32_t)(fp.source >> 32), fp.pair.upperKind,
                     (uint32_t)fp.source, fp.pair.lowerKind, fp.pair.g425Flags, 1u, (int32_t)j, u);
    }
    g730Select(ctx, u, false);
    g730NoteBody(ctx);
}
