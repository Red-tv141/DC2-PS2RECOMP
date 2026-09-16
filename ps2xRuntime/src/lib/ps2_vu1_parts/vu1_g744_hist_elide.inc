// ===== G744: DEAD G430 FMAC-HISTORY STORE ELISION INSIDE COMPILED BODIES =======================
//
// ⛔⛔⛔ THE PREMISE THAT BLOCKED THIS IS MEASURED FALSE. `vu1_g610_native_jit.inc`'s own comment
// at the push site has said, since G711:
//
//     "G711 measured this push at 0.525 ns/pair = 1.518 ms/f of VU1 busy on `dragon` (7.6% of it)
//      ... the largest single term in the compiled pair's memory traffic, three times the operand
//      and ACC round-trips combined. MOST OF IT IS DEAD: G612 makes every flag consumer a REGION
//      EXIT, so no pair inside a region can read the history and only the last <=8 FMAC pushes
//      before an exit are live. IT IS NOT ELIDED BECAUSE THE PRIZE DOES NOT CONVERT -- VU1 exceeds
//      every other pole on exactly one of the 23 canonical windows, and that window is at the
//      60 FPS cap."
//
// The mechanism half of that is right and is the design below. **The conversion half is what G743
// SS5b measured and refuted.** On `dragon`, `[G735:cpu] vu1CpuMs/f = 25.00` against a 26.3 ms/f
// frame -- VU1's real CPU is ~96 % of the whole frame, the GS worker idles ~14 ms/f over ~1,280
// kick stalls waiting for it, and a direct derivative puts VU1 conversion at **~1.0 there against
// 0.29 on `dungeon1`** (G743 SS5.1). G743 SS5b.1 names this exact sentence -- quoted from
// `g711_vu1_codegen_ceiling.inc` -- and calls it "false on this measurement".
//
// ⇒ A 1.518 ms/f VU1 term, known to be mostly dead, has been left in every shipped frame for the
// last thirty phases on a premise that no longer holds. That is the "old assumptions survived a
// major architectural change" family, and this is the fix.
//
// ---------------------------------------------------------------------------------------------
// WHAT IS ELIDED, AND WHY IT IS BIT-IDENTICAL RATHER THAN MERELY EQUIVALENT
//
// The emitted push is three stores plus the address arithmetic for one of them:
//
//     movups [r9+r10+HVEC],  histReg       ; 16 B
//     lea    rax, [r8 + pairIndexInBlock]
//     mov    [r9+r10+HSTAMP], rax          ; 8 B
//     mov    word [r9+r10+HDEST], imm16    ; 2 B
//     add    r10, HENT                     ; cursor
//     and    r10, (G430_HIST*HENT)-1
//
// ⭐ THE RULE. Elide the three stores (and the `lea`) at pair `j` **iff at least `G430_HIST` (8)
// further history-pushing pairs are guaranteed to execute before control can leave this body by
// ANY path.** Then every one of the 8 ring slots -- including the slot `j` would have written -- is
// overwritten by a later push before any exit, so at every exit the ring is byte-for-byte what the
// unelided body would have left.
//
// ⭐ THE CURSOR ADVANCE IS KEPT. `add`/`and` are two ALU ops with no memory traffic, and keeping
// them is what makes the argument above about SLOTS rather than about semantics: the ring walk is
// unchanged, so the later pushes land in exactly the slots they always did. G612 precondition 5
// ("the region keeps the ring cursor exact") therefore still holds verbatim, and
// `DC2_G610_VERIFY=1` / `DC2_G612_VERIFY=1` -- which compare the whole `VU1State` **plus all eight
// G430 entries** plus all of VU data RAM after every body -- stay valid as written. They are a real
// test of this change, not a formality: an off-by-one in the analysis moves a live entry and the
// oracle sees it entry by entry.
//
// ⭐ NO CONSUMER CAN OBSERVE THE RING MID-BODY, and that is stated in the tree rather than assumed:
// `vu1_g612_native_region.inc:625` -- "G490 blocks, G610 blocks and G612 regions all EXCLUDE flag
// readers" -- and `vu1_g421_fast_upper.inc:501` -- "`g430ResolveAt` scans all G430_HIST entries by
// STAMP and never reads `head`". The one in-region flag-adjacent op, G651's FCGET, reads `st.clip`
// and nothing else (`vu1_g612_native_region.inc:607-627`).
//
// ---------------------------------------------------------------------------------------------
// THE ANALYSIS IS PER STRAIGHT-LINE RUN, AND FOR REGIONS THAT IS THE WHOLE STORY
//
//   * A **G610 block** is straight-line with a single `ret`. "Pushes after j" is a backward count.
//   * A **G612 region** is a CFG -- but every basic block begins with the cycle-budget test
//     (`lea rax,[r8+pairs]; cmp rax,rsi; ja <exit>`), so a BB ENTRY IS A REACHABLE EXIT. The
//     minimum over all paths therefore never crosses a BB boundary, and the exact analysis
//     collapses to the same backward count within each BB. That is a restriction on the YIELD, not
//     an approximation: counting across the boundary would be unsound for exactly the budget exit.
//
// ⚠️ The terminator pair and its delay slot (plan indices >= `plain`) are never elided. They are
// emitted through a different index path and are within 8 of the block end anyway.
//
// ⚠️ REFUSALS ARE NOT AFFECTED. The `g611Ops` refusal inside the MIN/MAX partial-dest arm still
// runs when the push is elided, because it decides whether the BODY COMPILES. Letting elision
// change admission would change `pairs/call`, and a VU1 rate gate is only readable at matched
// `pairs/call` (G711's rule -- the first attempt at this class changed the workload and read
// 42-49 % "slower" while removing work).
//
// Levers:
//   DC2_G744_NO_HISTELIDE=1   rollback -- emit every push exactly as before
//   DC2_G744_HIST_STAT=1      [G744:hist] translate-time census (no runtime cost: the counters are
//                             bumped by the EMITTER, once per translated pair, never per execution)

#ifndef DC2_G744_HIST_DEFAULT_ON
#define DC2_G744_HIST_DEFAULT_ON 1
#endif

bool g744HistElideOn()
{
    static const bool s_on = (DC2_G744_HIST_DEFAULT_ON != 0) &&
                             !f50_12_vu_env_flag("DC2_G744_NO_HISTELIDE");
    return s_on;
}

bool g744HistStatOn()
{
    static const bool s_on = f50_12_vu_env_flag("DC2_G744_HIST_STAT");
    return s_on;
}

// Translate-time census. Bumped once per translated pair in the cold rebuild pass.
thread_local uint64_t s_g744HistPairs = 0ull;   // pairs offered to the emitter
thread_local uint64_t s_g744HistPushes = 0ull;  // of those, ones that push G430 history
thread_local uint64_t s_g744HistElided = 0ull;  // of those pushes, ones emitted without stores
thread_local uint64_t s_g744HistRuns = 0ull;    // straight-line runs planned
thread_local uint64_t s_g744HistBlocks = 0ull;  // plans for a G610 block
thread_local uint64_t s_g744HistBbs = 0ull;     // plans for a G612 basic block

// The plan for the CURRENT straight-line run, indexed by the same `pairIndexInBlock` the emitter
// is handed. A G612 region re-plans per basic block, and every BB restarts the index at 0, so the
// plan is cleared on every call -- an unplanned index can only ever read 0 (do not elide).
constexpr uint32_t G744_PLAN_BITS = 512u; // >= G612_MAX_PAIRS and any G610 block
thread_local uint64_t s_g744Plan[G744_PLAN_BITS / 64u] = {};
thread_local bool s_g744PlanArmed = false;

inline void g744PlanClear()
{
    for (uint32_t i = 0u; i < (G744_PLAN_BITS / 64u); ++i)
        s_g744Plan[i] = 0ull;
    s_g744PlanArmed = false;
}

// Does this upper kind reach the G430 push in `g610EmitUpper`?
//
// ⚠️ THIS MUST BE A SUBSET OF THE EMITTER'S OWN BEHAVIOUR, NEVER A SUPERSET. Over-counting pushes
// would let a pair be elided whose slot is NOT overwritten before the exit. The predicate is
// therefore read off the same descriptor table the emitter switches on: VNOP (0x6f) returns before
// anything, `cls == 0` refuses the body, `cls == 8` (G611 CLIPw) returns at its own arm -- its
// comment says "the only compiled upper that writes neither VF nor ACC, pushes no G430 history" --
// and `cls >= 3` (ITOF/FTOI/ABS/...) returns at the `d.cls >= 3` arm, whose comment says "None of
// these five writes MAC/STATUS or the G430 history". What is left, `cls` 1 (-> VF[fd]) and 2
// (-> ACC), is exactly what falls through to the push. This is the same shape as the emitter's own
// `readsQ` test (`cls != 0 && cls < 3`).
inline bool g744KindPushesHist(uint8_t kind)
{
    if (kind == 0x6fu)
        return false;
    const uint8_t cls = g421DescTableConst.e[kind].cls;
    return cls >= 1u && cls <= 2u;
}

// Plan one straight-line run of `scanCount` pairs starting at `firstIndex` in the fused-pair cache.
// Plan index `j` corresponds to cache slot `firstIndex + j`, which is exactly the correspondence
// both emitters use.
//
// ⭐ G745 generalised this in two directions, both of which are strict no-ops at the old call
// shape (`g744PlanRun` below passes `markCount == scanCount` and `seedAfter == 0`):
//
//   * `seedAfter` — pushes ALREADY GUARANTEED to execute after this run ends, before control can
//     leave the body by any path. For a G610 block and for the G744 per-BB form this is 0 (the run
//     ends at an exit). For a G612 basic block whose entry carries no budget test it is
//     `g745MinPushes`'s `outMin[k]`, which is what lets the count cross a block boundary.
//     ⛔ IT MUST BE A LOWER BOUND OVER **ALL** PATHS — a minimum, never an average or a sample.
//   * `markCount` — how many leading indices may be MARKED. Pairs in `[markCount, scanCount)` are
//     counted (they execute, so they overwrite ring slots) but never elided: they are the branch
//     terminator and its delay slot, which reach the emitter through a different index path.
void g744PlanRunEx(const G479FusedPair *fused, uint32_t firstIndex, uint32_t scanCount,
                   uint32_t markCount, uint32_t seedAfter, bool isBlock)
{
    g744PlanClear();
    if (!g744HistElideOn() || fused == nullptr || scanCount == 0u)
        return;
    if (scanCount > G744_PLAN_BITS)
        scanCount = G744_PLAN_BITS; // never plan an index the bitmap cannot hold
    if (markCount > scanCount)
        markCount = scanCount;
    ++s_g744HistRuns;
    if (isBlock)
        ++s_g744HistBlocks;
    else
        ++s_g744HistBbs;
    uint32_t after = seedAfter > (uint32_t)G430_HIST ? (uint32_t)G430_HIST : seedAfter;
    for (uint32_t k = scanCount; k-- > 0u;)
    {
        const bool pushes = g744KindPushesHist(fused[firstIndex + k].pair.upperKind);
        if (pushes && after >= (uint32_t)G430_HIST)
        {
            if (k < markCount)
            {
                s_g744Plan[k >> 6] |= 1ull << (k & 63u);
                s_g744PlanArmed = true;
            }
        }
        if (pushes && after < (uint32_t)G430_HIST)
            ++after;
    }
}

void g744PlanRun(const G479FusedPair *fused, uint32_t firstIndex, uint32_t count, bool isBlock)
{
    g744PlanRunEx(fused, firstIndex, count, count, 0u, isBlock);
}

// Read by the emitter at the push site. `false` for every index outside a planned run.
inline bool g744ElideAt(uint32_t pairIndex)
{
    if (!s_g744PlanArmed || pairIndex >= G744_PLAN_BITS)
        return false;
    return (s_g744Plan[pairIndex >> 6] >> (pairIndex & 63u)) & 1ull;
}

// RAII: a leaked plan would let the NEXT body elide by an index that means something else there.
// Every emitter entry point that can return early is covered by one of these.
struct G744PlanScope
{
    G744PlanScope() {}
    ~G744PlanScope() { g744PlanClear(); }
};

void g744HistReport()
{
    if (!g744HistStatOn())
        return;
    std::fprintf(stderr,
                 "[G744:hist] arm=%d runs=%llu (blocks=%llu bbs=%llu) pairs=%llu pushes=%llu "
                 "elided=%llu (%.1f%% of pushes)\n",
                 g744HistElideOn() ? 1 : 0,
                 (unsigned long long)s_g744HistRuns, (unsigned long long)s_g744HistBlocks,
                 (unsigned long long)s_g744HistBbs, (unsigned long long)s_g744HistPairs,
                 (unsigned long long)s_g744HistPushes, (unsigned long long)s_g744HistElided,
                 s_g744HistPushes ? 100.0 * (double)s_g744HistElided / (double)s_g744HistPushes
                                  : 0.0);
}
