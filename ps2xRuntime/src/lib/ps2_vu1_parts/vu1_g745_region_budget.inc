// ===== G745: HOISTING THE G612 PER-BASIC-BLOCK CYCLE-BUDGET TEST =============================
//
// ⛔⛔⛔ THE TEST THIS FILE REMOVES CAN NEVER FIRE, AND THAT IS MEASURED, NOT ASSUMED.
//
//   * the VU1 cycle budget is `dc2Vu1CycleBudget()` = **65,536** (runtime_mtvu_and_env.inc:1464,
//     overridable only by `DC2_G22_VU1_CYCLES`), and the budget handed to a region is
//     `maxCycles - cycle`, so it is never below ~64,200;
//   * `cycle` advances one per PAIR (`vu1_g435_pair_loop.inc:190`), and `[G483:runprof]` reads
//     **`pairs/call` = 1,295..1,308** on `dragon`; `[G612:reg] max` — the largest number of pairs
//     any single region run has ever consumed — is **1,275**.
//
// ⇒ `consumed + bb.pairs > budget` is a comparison of a number that never exceeds ~1,300 against
// one that is never below ~64,200. Every basic block in every region pays `lea`/`cmp`/`jcc` for it,
// on the thread that is 93–98 % of the frame in the three worst windows of the G744 board.
//
// ⭐⭐⭐ AND THE BRANCH IS THE CHEAP HALF. `vu1_g744_hist_elide.inc` elides a dead G430 history push
// only when **8 further pushing pairs are guaranteed before control can leave the body by ANY
// path** — and G744 had to treat a basic-block ENTRY as a reachable exit *because of this test*:
//
//     "every basic block begins with the cycle-budget test ... so a BB ENTRY IS A REACHABLE EXIT,
//      and the minimum over all paths therefore never crosses a BB boundary."
//
// Regions carry ~60 % of all compiled pairs and their basic blocks average ~4 executed pairs, so
// that restriction takes region elision to approximately ZERO: the `[G744:hist]` census reads
// 40.8 % of pushes elided, and essentially all of it comes from G610 blocks (`[G490] avg = 15.94`,
// ceiling `(avg-8)/avg` = 49.8 %). The budget test is therefore not a 3-instruction tax — it is
// what keeps ~1 ms/f of KNOWN-DEAD stores in the frame.
//
// ---------------------------------------------------------------------------------------------
// WHAT REPLACES IT
//
// A basic block keeps the test only if it is a **back-edge target** (a loop header). Every cycle in
// any digraph contains at least one back edge of any DFS forest over it, so checking at every
// back-edge target guarantees every cycle still contains a check and the region cannot spin.
//
// At a check point `k` the test compares against `worst[k]` — the maximum number of pairs along any
// path from `k` up to (and excluding) the next check point or region exit — instead of `bb.pairs`.
// The remaining checks are therefore CONSERVATIVE: the region may suspend slightly earlier than the
// per-block form would have. `maxCycles` is an architectural suspension edge (this file's §6
// precondition), so an early suspension is always sound; the caller resumes at the pc the exit stub
// writes, exactly as it does today.
//
// ⭐ THE ENTRY EDGE IS HANDLED BY THE CALLER, NOT BY A THUNK CHECK. `g612RunRegion` already refuses
// to call a region whose entry block does not fit (`if (budget < s.pairs)`); that precheck now
// carries `worst[entry]` instead of `bb.pairs`. So entering at a non-header block needs no test at
// all, which is what makes its entry stop being an exit.
//
// ⇒ AFTER THIS CHANGE A NON-HEADER BASIC BLOCK HAS NO EXIT AT ITS ENTRY, and the G744 analysis can
// be what it always wanted to be: a backward "minimum pushes to any exit" dataflow over the whole
// region CFG (`g745MinPushes` below), seeded into each block's plan.
//
// ---------------------------------------------------------------------------------------------
// THE DATAFLOW (backward, meet = min, saturating at G430_HIST)
//
//   outMin[k] = 0                            if control can leave the region at k's terminator
//             = min over internal succs s of inMin[s]    otherwise
//   inMin[k]  = 0                            if k keeps a budget check (its entry is an exit)
//             = sat8(pushes(k) + outMin[k])   otherwise
//
// Solved as a GREATEST fixed point — every value starts at 8 (optimistic) and only ever decreases,
// so the iteration terminates in at most 8*nBb rounds and a loop with no exit at all correctly
// keeps 8 (nothing can observe the ring on a path that never leaves).
//
// ⛔ THE SEED MUST BE A LOWER BOUND, NEVER AN UPPER ONE. `pushes(k)` counts a pair only through
// `g744KindPushesHist`, which is documented there as a SUBSET of the emitter's own behaviour. The
// terminator pair and its delay slot are COUNTED (they execute and they push) but never MARKED —
// they are emitted through a different index path, exactly as in G744.
//
// Levers:
//   DC2_G745_NO_BUDGETHOIST=1  rollback — per-BB budget test and per-BB elision plan, i.e. G744
//   DC2_G745_STAT=1            `[G745:hoist]` translate-time census (emitter-side counters only)
// ==============================================================================================

// ⛔ DEFAULT OFF, AND THAT IS A MEASURED DISPOSITION, NOT A BRING-UP LEFTOVER (Rule G744-3).
//
// The mechanism is correct and it does what it says: `[G745:hoist]` reads `checksKept = 47 of 660`
// (92.9 % of the per-block tests gone) with `failed = 0`, and `[G612:reg] codeKiB` falls 258.3 ->
// 240.0. `DC2_G612_VERIFY=1` + `DC2_G610_VERIFY=1` on `dragon` report **`bad = 0` over 2,000,000
// region and 2,000,000 block verifications (77 M pairs)** — the oracle compares the whole
// `VU1State`, ALL EIGHT G430 ring entries and all of VU data RAM after every body.
//
// ⛔ IT DOES NOT CONVERT. Three order-balanced ABBA blocks of `tools/g745_runprof_gate.ps1` on
// `dragon`, same binary, env only, at `pairs/call` identity of 0.02 %:
//
//     block   effect ns/pair   control drift   admissible
//       1        -0.2043           0.4406          no
//       2        -0.0845           0.2755          no
//       3        +0.0355           0.3152          no
//       4        +0.5366           0.0366         ⭐ YES  (sub-blocks +0.3851 / +0.6881, agree)
//
// ⛔ THE ONLY ADMISSIBLE BLOCK SAYS THE HOIST IS A REGRESSION. Three blocks are inadmissible on
// their own drift and straddle zero; the fourth, whose control drift is a clean 0.0366, reads
// **+0.5366 ns/pair with both sub-blocks agreeing in sign**. A lever whose sign flips between
// blocks and whose one clean block is positive does not ship. This is Rule 46 territory — the
// change is a pure CODE-LAYOUT change inside a PGO'd image, and layout is exactly what a
// within-binary env A/B cannot hold constant.
//
// ⭐⭐⭐ AND THE BALLAST SETTLES IT ON A NUMBER RATHER THAN ON NOISE. `DC2_G745_BAL=8`, hoist OFF in
// both arms, one ABBA on `dragon` at `pairs/call` identity of 0.01 %:
//
//     control 8.5621  ->  candidate 8.7062  =  +0.1441 ns/pair   control drift 0.1324  ADMISSIBLE
//     => ONE extra test at EVERY basic block costs  0.1441 / 8 = **0.0180 ns/pair**
//     => the hoist removes 613 of 660, so its whole ceiling is **0.0167 ns/pair = 0.05 ms/f**
//        on `dragon` (2.87 M pairs/f)
//
// That is ~30x BELOW the 0.28-0.44 ns/pair drift of the rate gate, which is why four ABBA blocks
// could only measure layout noise around it. The physics agrees: `lea`/`cmp`/`ja` is two fused uops
// on a branch that is never taken and never mispredicted, ~0.12 ns per check, entirely off the
// dependency chain the pair loop is actually limited by.
//
// ⇒ CLOSED ON CONVERSION, not on difficulty. Ships OFF, armed with `DC2_G745_BUDGETHOIST=1`.
// ⛔ DO NOT RE-OPEN `plans/phase-G744-plan.md` §6 ("merging the budget check of a BB into its unique
// predecessor ... would both remove a check and lengthen the elision runs ... wants its own phase").
// BOTH HALVES OF THAT SENTENCE ARE NOW MEASURED FALSE: the elision half moves 40.8 % -> 41.0 %, and
// the check half is worth 0.05 ms/f.
//
// ⭐⭐ AND THE ELISION HALF IS A MEASURED NULL, WHICH REFUTES THE PREMISE THIS WAS BUILT ON.
// `plans/phase-G744-plan.md` §6 names merging the per-block budget check as "the next lever, not a
// bigger ring", on the reasoning that it "would both remove a check and LENGTHEN THE ELISION RUNS".
// It does not. With the check hoisted, `[G744:hist] elided` moves **3787 -> 3810 of 9284 pushes
// (40.8 % -> 41.0 %)**, and the reason is in this file's own census: only **147 of 660** blocks gain
// any cross-block guarantee at all and the average guarantee is **3.96**, against a threshold of 8.
// Region CFGs exit too densely for the count to reach `G430_HIST` no matter where the checks sit —
// so what limited region elision was never the budget test. ⇒ THE DEAD-HISTORY ELISION IS AT ITS
// CEILING AT G744's 40.8 %. Do not re-open it from this direction.
#ifndef DC2_G745_HOIST_DEFAULT_ON
#define DC2_G745_HOIST_DEFAULT_ON 0
#endif

bool g745HoistOn()
{
    static const bool s_on = (DC2_G745_HOIST_DEFAULT_ON != 0)
                                 ? !f50_12_vu_env_flag("DC2_G745_NO_BUDGETHOIST")
                                 : f50_12_vu_env_flag("DC2_G745_BUDGETHOIST");
    return s_on;
}

bool g745StatOn()
{
    static const bool s_on = f50_12_vu_env_flag("DC2_G745_STAT");
    return s_on;
}

// ⭐⭐ G745 BALLAST — THE PRICE OF THE PER-BLOCK BUDGET TEST, MEASURED INSTEAD OF ARGUED.
//
// Three order-balanced ABBA blocks of `tools/g745_runprof_gate.ps1` on `dragon` put the hoist at
// -0.204 / -0.084 / +0.036 ns/pair against a control drift of 0.28-0.44 -- i.e. the lever is BELOW
// THIS ROUTE'S FLOOR, which is not the same statement as "the checks are free". Rule 88's remedy is
// ballast: re-emit the SAME test N extra times at each block that keeps one, and divide.
//
// ⭐ IT IS EXACT BY CONSTRUCTION, NOT MERELY EQUIVALENT. The extra copies are byte-identical to the
// real test -- same `lea` displacement, same `cmp`, same `ja` to the same exit stub -- and the test
// is pure (it writes only RAX, which the block body re-defines before any use). So if the real test
// falls through, every copy falls through; if it exits, the first copy exits at exactly the pc the
// real one would have. `pairs/call`, `[G612:reg] avg` and the rendered image are invariant.
//
// ⛔ MEASUREMENT ONLY. Arm it with the hoist ROLLED BACK (`DC2_G745_NO_BUDGETHOIST=1`), so the
// population priced is exactly the one the hoist removes: one test at every basic block.
//
//   DC2_G745_BAL=<n>   extra copies of the budget test per basic block (use 8)
uint32_t g745BalN()
{
    static const uint32_t s_n = [] {
        const char *v = std::getenv("DC2_G745_BAL");
        if (v == nullptr || *v == '\0')
            return 0u;
        const unsigned long n = std::strtoul(v, nullptr, 0);
        return (uint32_t)(n > 32ul ? 32ul : n); // bounded: `fixes[]` is sized for it
    }();
    return s_n;
}

// Translate-time census. Bumped by the EMITTER, never by an executed pair.
thread_local uint64_t s_g745Regions = 0ull;   // regions planned with the hoist armed
thread_local uint64_t s_g745Bbs = 0ull;       // basic blocks in them
thread_local uint64_t s_g745Checks = 0ull;    // of those, blocks that KEEP a budget test
thread_local uint64_t s_g745Cyclic = 0ull;    // regions that contain at least one back edge
thread_local uint64_t s_g745Failed = 0ull;    // regions where the analysis failed closed
thread_local uint64_t s_g745SeedSum = 0ull;   // sum of the per-block elision seeds granted
thread_local uint64_t s_g745SeedBbs = 0ull;   // blocks that received a NON-ZERO seed

// One region's analysis result. Lives at namespace scope for the same reason `s_g612Plan` does
// (G609: a function-local `static thread_local` is a per-access TLS guard).
struct G745Analysis
{
    bool ok = false;                     // false -> caller must use the per-BB form
    bool check[G612_MAX_BB] = {};        // this block keeps its cycle-budget test
    uint16_t worst[G612_MAX_BB] = {};    // pairs to the next check point / region exit, worst case
    uint8_t inMin[G612_MAX_BB] = {};     // pushes guaranteed from this block's ENTRY before an exit
    uint8_t outMin[G612_MAX_BB] = {};    // ... from this block's terminator onward
};
thread_local G745Analysis s_g745;

// Internal successors of block `k`, and whether control can leave the region at its terminator.
// Mirrors the emission in `g612EmitRegion` edge for edge:
//   T_EXIT / T_JR      -> always leaves (a `ret` pc, or a dynamic target)
//   T_FALL / T_JMP     -> one static edge; leaves when that slot is not a block of this region
//   T_BR               -> two static edges; leaves when EITHER is outside
void g745Succ(const G612Plan &p, uint32_t k, int &s0, int &s1, bool &exits)
{
    const G612Bb &bb = p.bb[k];
    s0 = -1;
    s1 = -1;
    exits = false;
    switch (bb.term)
    {
    case G612_T_EXIT:
    case G612_T_JR:
        exits = true;
        break;
    case G612_T_FALL:
    {
        const int d = g612FindBb(p, bb.fallSlot);
        if (d < 0)
            exits = true;
        else
            s0 = d;
        break;
    }
    case G612_T_JMP:
    {
        const int d = g612FindBb(p, bb.takenSlot);
        if (d < 0)
            exits = true;
        else
            s0 = d;
        break;
    }
    case G612_T_BR:
    {
        const int a = g612FindBb(p, bb.takenSlot);
        const int b = g612FindBb(p, bb.fallSlot);
        if (a < 0 || b < 0)
            exits = true;
        if (a >= 0)
            s0 = a;
        if (b >= 0)
            s1 = b;
        break;
    }
    default:
        exits = true; // unknown terminator: fail closed, it is an exit
        break;
    }
}

// Pairs of block `k` whose upper reaches the G430 push. Counts the terminator pair and the delay
// slot for a branch terminator (`bb.pairs == bb.plain + 2`), because both execute.
uint32_t g745PushesIn(const G612Plan &p, uint32_t k)
{
    const G612Bb &bb = p.bb[k];
    uint32_t n = 0u;
    for (uint32_t j = 0u; j < bb.pairs; ++j)
    {
        const uint32_t sl = bb.start + j;
        if (sl >= G612_SLOTS)
            break;
        if (g744KindPushesHist(s_g410Cache.fused[sl].pair.upperKind))
            ++n;
    }
    return n;
}

// DFS over the whole block graph (a region may be ENTERED at any block, so the forest is rooted at
// every unvisited node). An edge to a GRAY node is a back edge; its target becomes a check point.
// Iterative: G612_MAX_BB is 48, but recursion in the translator is a habit worth not forming.
void g745MarkHeaders(const G612Plan &p, G745Analysis &a)
{
    uint8_t color[G612_MAX_BB] = {}; // 0 white, 1 gray, 2 black
    uint32_t stack[G612_MAX_BB];
    uint8_t edge[G612_MAX_BB]; // which successor of stack[top] is next
    for (uint32_t root = 0u; root < p.nBb; ++root)
    {
        if (color[root] != 0u)
            continue;
        uint32_t top = 0u;
        stack[0] = root;
        edge[0] = 0u;
        color[root] = 1u;
        for (;;)
        {
            const uint32_t k = stack[top];
            int s0 = -1, s1 = -1;
            bool ex = false;
            g745Succ(p, k, s0, s1, ex);
            const int succ[2] = {s0, s1};
            bool descended = false;
            while (edge[top] < 2u)
            {
                const int s = succ[edge[top]];
                ++edge[top];
                if (s < 0)
                    continue;
                if (color[s] == 1u)
                {
                    a.check[s] = true; // back edge -> loop header
                    continue;
                }
                if (color[s] == 0u)
                {
                    color[s] = 1u;
                    ++top;
                    stack[top] = (uint32_t)s;
                    edge[top] = 0u;
                    descended = true;
                    break;
                }
            }
            if (descended)
                continue;
            color[k] = 2u;
            if (top == 0u)
                break;
            --top;
        }
    }
}

// worst[k] = bb[k].pairs + max over internal successors s of (check[s] ? 0 : worst[s]).
// Edges into check points are cut, and every cycle contains a check point, so the remaining graph
// is acyclic. The `gray` guard proves that rather than trusting it: a residual cycle fails closed.
bool g745WorstPairs(const G612Plan &p, G745Analysis &a)
{
    uint8_t state[G612_MAX_BB] = {}; // 0 unknown, 1 in progress, 2 done
    uint32_t stack[G612_MAX_BB];
    uint8_t edge[G612_MAX_BB];
    for (uint32_t root = 0u; root < p.nBb; ++root)
    {
        if (state[root] == 2u)
            continue;
        uint32_t top = 0u;
        stack[0] = root;
        edge[0] = 0u;
        state[root] = 1u;
        a.worst[root] = p.bb[root].pairs;
        for (;;)
        {
            const uint32_t k = stack[top];
            int s0 = -1, s1 = -1;
            bool ex = false;
            g745Succ(p, k, s0, s1, ex);
            const int succ[2] = {s0, s1};
            bool descended = false;
            while (edge[top] < 2u)
            {
                const int s = succ[edge[top]];
                ++edge[top];
                if (s < 0 || a.check[s])
                    continue; // exit edge, or an edge into a check point: contributes 0
                if (state[s] == 1u)
                    return false; // residual cycle -> fail closed
                if (state[s] == 0u)
                {
                    state[s] = 1u;
                    a.worst[s] = p.bb[s].pairs;
                    ++top;
                    stack[top] = (uint32_t)s;
                    edge[top] = 0u;
                    descended = true;
                    break;
                }
                // already done: fold it in now
                const uint32_t cand = (uint32_t)p.bb[k].pairs + (uint32_t)a.worst[s];
                if (cand > (uint32_t)a.worst[k])
                {
                    if (cand > G612_MAX_PAIRS * 2u)
                        return false; // structurally impossible; refuse rather than truncate
                    a.worst[k] = (uint16_t)cand;
                }
            }
            if (descended)
                continue;
            state[k] = 2u;
            if (top == 0u)
                break;
            --top;
            // fold the finished child into its parent
            const uint32_t par = stack[top];
            const uint32_t cand = (uint32_t)p.bb[par].pairs + (uint32_t)a.worst[k];
            if (cand > (uint32_t)a.worst[par])
            {
                if (cand > G612_MAX_PAIRS * 2u)
                    return false;
                a.worst[par] = (uint16_t)cand;
            }
        }
    }
    return true;
}

// The backward "minimum pushes before any exit" dataflow. Greatest fixed point: start optimistic
// and only ever decrease, so it terminates and a never-exiting loop keeps G430_HIST.
void g745MinPushes(const G612Plan &p, G745Analysis &a)
{
    uint32_t pushes[G612_MAX_BB];
    for (uint32_t k = 0u; k < p.nBb; ++k)
    {
        pushes[k] = g745PushesIn(p, k);
        a.inMin[k] = (uint8_t)G430_HIST;
        a.outMin[k] = (uint8_t)G430_HIST;
    }
    for (uint32_t round = 0u; round <= (uint32_t)G430_HIST * (p.nBb + 1u); ++round)
    {
        bool changed = false;
        for (uint32_t k = 0u; k < p.nBb; ++k)
        {
            int s0 = -1, s1 = -1;
            bool ex = false;
            g745Succ(p, k, s0, s1, ex);
            uint32_t out = ex ? 0u : (uint32_t)G430_HIST;
            if (!ex)
            {
                if (s0 >= 0 && (uint32_t)a.inMin[s0] < out)
                    out = a.inMin[s0];
                if (s1 >= 0 && (uint32_t)a.inMin[s1] < out)
                    out = a.inMin[s1];
            }
            uint32_t in = 0u;
            if (!a.check[k])
            {
                in = pushes[k] + out;
                if (in > (uint32_t)G430_HIST)
                    in = (uint32_t)G430_HIST;
            }
            if (out != (uint32_t)a.outMin[k])
            {
                a.outMin[k] = (uint8_t)out;
                changed = true;
            }
            if (in != (uint32_t)a.inMin[k])
            {
                a.inMin[k] = (uint8_t)in;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
}

// Run the whole analysis for one planned region. On `false` the caller emits the G744 form.
bool g745Analyse(const G612Plan &p)
{
    G745Analysis &a = s_g745;
    a = G745Analysis{};
    if (!g745HoistOn() || p.nBb == 0u || p.nBb > G612_MAX_BB)
        return false;
    g745MarkHeaders(p, a);
    if (!g745WorstPairs(p, a))
    {
        if (g745StatOn())
            ++s_g745Failed;
        return false;
    }
    g745MinPushes(p, a);
    a.ok = true;
    if (g745StatOn())
    {
        ++s_g745Regions;
        s_g745Bbs += p.nBb;
        bool cyclic = false;
        for (uint32_t k = 0u; k < p.nBb; ++k)
        {
            if (a.check[k])
            {
                ++s_g745Checks;
                cyclic = true;
            }
            if (a.outMin[k] != 0u)
            {
                ++s_g745SeedBbs;
                s_g745SeedSum += a.outMin[k];
            }
        }
        if (cyclic)
            ++s_g745Cyclic;
    }
    return true;
}

void g745Report()
{
    if (!g745StatOn())
        return;
    std::fprintf(stderr,
                 "[G745:hoist] arm=%d regions=%llu bbs=%llu checksKept=%llu (%.1f%% of bbs) "
                 "cyclicRegions=%llu failed=%llu | seededBbs=%llu avgSeed=%.2f\n",
                 g745HoistOn() ? 1 : 0, (unsigned long long)s_g745Regions,
                 (unsigned long long)s_g745Bbs, (unsigned long long)s_g745Checks,
                 s_g745Bbs ? 100.0 * (double)s_g745Checks / (double)s_g745Bbs : 0.0,
                 (unsigned long long)s_g745Cyclic, (unsigned long long)s_g745Failed,
                 (unsigned long long)s_g745SeedBbs,
                 s_g745SeedBbs ? (double)s_g745SeedSum / (double)s_g745SeedBbs : 0.0);
}
