// G490 - WIDENED BLOCK RUN EXECUTOR, on the PROMOTED (fixed + fused + wide-VF0) loop body.
//
// WHY THIS EXISTS, and why it is not G475.
//
// G489's `/MAP` + PC-sample overlay measured the pair loop at ~63 cycles / ~75 retired
// instructions per pair, of which **54.7% is the per-pair ENVELOPE** (pc-oob test, index math,
// record load, upper/lower split, flag extract, sequential-PC / branch fold / pc store / ebit /
// loop condition) and 20.7% is field extraction from words that never change for a given PC.
// 60 FPS needs 1.55x on this thread. The structural answer is to pay the envelope ONCE PER RUN
// of covered pairs instead of once per pair — which is the premise the whole block-translation
// arc rests on. This file prices that premise at the SOURCE level before anything is emitted.
//
// G475 already built a run executor and G478 measured it +0.34 ms/f SLOWER, which the ROADMAP
// asked G490 to understand first. Two things were found:
//
//   1. **The G475 trace copy is STRUCTURALLY UNREACHABLE on the current binary.** Its two
//      selector arms in vu1_upper_opcodes.inc sit BELOW `else if (g443Fixed && g479LeanFetch)`,
//      and `g479LeanFetch` has been default-ON since G479 was promoted. Arming
//      `DC2_G419_AB=vnoptrace` today cannot put the trace into the promoted loop; it can only
//      drop the whole run onto a different, slower copy. This is exactly the G482/G484
//      "structurally unmeasurable lever" class, one arc later.
//   2. Therefore G478's `+0.34` describes a PRE-G479, PRE-G485 body: a non-fused split fetch and
//      four narrow VF0 stores, in both arms. It is not evidence about today's loop.
//
// So this is the re-price the ROADMAP asked for, done the only way it can be done — as a copy of
// the SHIPPED body (DC2_G484_WIDEVF0 1 + DC2_G479_FUSED 1) with the run hook added, so the
// control arm is the promoted default and nothing else differs.
//
// WHAT IS WIDER THAN G475. G475 covered `upperKind == 0x6f` (VNOP) only — 34.5% of MAP-0 pairs,
// and the half of the stream that carries no upper work at all, i.e. the half where the envelope
// is the SMALLEST fraction of the pair. G490 covers **every upper class `g421FastUpper` executes**
// (all `cls != 0`), because the run body can call the same inlined SIMD upper with the same G430
// stamped-history push that the pair loop passes it. That is the class a block translator would
// emit, so this measures the arc's real premise instead of its easiest corner.
//
// EXACTNESS — what the elision rests on. A pair is eligible only when the per-pair machinery this
// loop performs around the two slots is PROVABLY INERT for it (g490BlockPairEligible):
//   * `pairFlags == 0`     -> no E-bit (no latch, no drain) and no I-bit (the lower word is not a
//                             float immediate), so the LOI arm and the ebit test are no-ops.
//   * `!G426_PF_PRESTALL`  -> the G239 scalar-prestall block is guarded by this bit; clear means
//                             the lower is neither a FDIV/EFU producer nor WAITQ/WAITP.
//   * `!G427_PF_FLAGREAD`  -> under the promoted `g430Hist` model the MAC/STATUS line is NOT
//                             advanced per pair; the only per-pair work is `g430ResolveAt` for a
//                             consumer pair. Clear means there is none.
//   * `!G426_PF_HAZ`       -> the G139 pair-hazard save/restore/overlay is guarded by this bit.
//   * lower is a covered, NON-CONTROL, NON-FLAG kind -> `g422FastLower` executes it, and G489
//                             established from the complete `m_state.pc =` set in the source that
//                             only the ten branch/jump lowers can write pc. So `postExecPc ==
//                             currentPc`, the branch fold is dead, and the PC is sequential.
//   * upper is VNOP (the loop skips it) or `cls != 0` (`g421FastUpper` returns true).
// What remains per pair is then exactly: Q advance, P advance, upper, lower, VF0/VI0 re-pin,
// pc += 8 — which is what the block below executes, in that order.
//
// Verify: DC2_G490_VERIFY=1 re-derives every one of those predicates from the LIVE code words for
//         every pair of every executed block and aborts on the first disagreement (it also
//         re-reads the cached source, so a missed invalidation inside a block cannot hide).
// Stats:  DC2_G490_STAT=1 -> `[G490:block] runs= pairs= avg= cover=`.
// Arm:    DC2_G419_AB=blockrun (paired, hold 16 — it is a cache-decode arm) or DC2_G490_BLOCK=1.
extern int g490BlockRunArm();

// G499 - THE BLOCK EXECUTOR'S LOWER DISPATCH, brought level with the pair loop's.
//
// WHY. `g490ExecuteBlock` below calls `g422FastLower` for EVERY pair of every run. The pair loop
// has not done that since G480/G481: `DC2_G422_RUN_LOWER` (vu1_upper_opcodes.inc) skips the call
// outright when the kind is NOP — the callee's whole body for that case is `return;` — and runs
// the hot INTEGER kinds inline. Both are promoted default-ON, and neither ever reached this copy;
// G490 built the block around the ENVELOPE and inherited the pre-G480 dispatch for the two slots.
//
// SIZE (this session's `DC2_G421_CENSUS=1` on MAP-0, 1.18 G pairs):
//     lowerNop        = 39.5% of all pairs          <- pays a 5-argument call to do nothing
//     IADDI/IADDIU/IAND = 6.99 + 3.07 + 3.02 = 13.1% <- pays a call for three integer instructions
//     [G490:block] cover = 77.0%, avg run 7.81
// Every one of those kinds is block-eligible (NOP is covered, names no register, cannot set the
// hazard/prestall/flagread bits), so essentially all of that population runs THROUGH this
// function. G481's own measurement is why only the three INTEGER kinds are inlined here and the
// memory kinds (LQ/LQI/SQI, 16-byte staging + two memcpys) are left behind the call: inlining all
// seven read +0.50 ms/f in the pair loop. The same law applies to this body.
//
// EXACTNESS. `G481_LOWER_BODY_*` are the SAME tokens `g422FastLower`'s own cases expand
// (vu1_g422_fast_lower.inc:368/449/452), so an inlined kind executes the identical statements in
// the identical order; and skipping the call for NOP is skipping a function whose body is
// `case G422_LK_NOP: return;`. Nothing is re-ordered and no state is touched that was not touched
// before. The two arms are ONE template body, so they cannot drift.
//
// Arm:    DC2_G419_AB=blklower (paired, hold 16 — thread-level code shape, like blockrun)
// Kill:   DC2_G499_NO_BLKLOWER=1
// Stats:  DC2_G499_STAT=1 -> `[G499:blk] arm=<0|1|2>` one-shot proof of selection per arm.
//
// The SECOND mechanism this phase adds to the same body (the Q/P counters as block locals) is
// gated separately — DC2_G419_AB=qpblock, kill DC2_G499_NO_QPLOCAL=1 — so each is priced alone.
// Its exactness argument is at the template body below.
extern int g499BlkLowerArm();
extern int g499QpBlockArm();

// G500 - LET A BLOCK RUN END ON A CONTROL TRANSFER (and swallow the delay slot).
//
// WHY. `g490BlockPairEligible` excludes the ten branch/jump lowers outright, so on this route
// **8.88% of all pairs** (B 0.38 + BAL 0.82 + JR 0.86 + IBEQ 2.40 + IBNE 4.41 + IBLEZ 0.01, re-taken
// this session and identical to G499's) are executed by the pair loop at the full per-pair envelope
// AND terminate the run they end — G499 §2 put control transfer at ~69% of all run terminations,
// which is why the average run is 7.81 and not longer. The delay slot of a TAKEN branch is excluded
// too, for a second reason: the block hook is guarded by `pendingBranchTarget == NO_BRANCH`, so the
// one pair that consumes a pending target can never start a block however eligible it is.
//
// WHAT THE ARM DOES. A run that would ALREADY have formed a block may execute up to two more pairs:
// the control transfer that ended it, and (when that slot is itself block-eligible) its delay slot.
// Nothing else changes — the arm never CREATES a block, never merges two runs and never lengthens
// the plain part, so the number of block calls per frame is invariant and the gate prices exactly
// one thing: pairs moved out of the pair loop into a block that was going to run anyway.
//
// EXACTNESS. The tail pair executes the same statements in the same order as a plain block pair
// (Q advance, P advance, upper, lower, VF0/VI0 re-pin) with two additions, both of which reproduce
// the pair loop's own protocol verbatim:
//   * `st.pc` is written to the tail pair's own PC before its lower runs, because the branch bodies
//     derive their target from it and BAL/JALR write `(st.pc + 16) / 8` into VI. The pair loop has
//     that value live for every pair; the block writes it once.
//   * the branch OUTCOME is read back exactly as the pair loop's tail reads it — `st.pc` different
//     from the tail pair's PC means taken, and the target is `st.pc + 8` with the same wrap. That
//     is the identical test, so even the degenerate `B 0` encoding (target == the delay slot, which
//     the pair loop treats as not-taken) behaves the same in both arms.
// The delay slot, when it is taken into the block, is a plain eligible pair by construction, and
// nothing in `g421FastUpper` or the non-branch `g422FastLower` kinds reads `st.pc`, so the branch's
// write to it cannot be observed before the caller consumes it.
//
// Arm:    DC2_G419_AB=blkbranch (paired, hold 16 — thread-level code shape, like blockrun/blklower)
// Kill:   DC2_G500_NO_BLKBRANCH=1
// Stats:  DC2_G500_STAT=1 -> `[G500:inv] arm=<0|1>` per-arm workload invariant (blocks/f, pairs,
//         avg run, tail1/tail2/taken counts) printed from the SAME run, plus `[G499:blk] arm=3`.
// Verify: DC2_G490_VERIFY=1 re-derives the tail predicate from the live code words as well.
extern int g500BlkBranchArm();

// G502 - WHAT A BLOCK COSTS TO ENTER, AND WHAT IS LEFT OUTSIDE IT.
//
// Two questions this arc has never answered, both about the SAME loop, both answered by the census
// and the ceiling probe below BEFORE any lever is written (G497 §2 / G500 §8):
//
//  1. THE ROUND TRIP. `cover=88.99%` at `avg=9.15` means ~157 k block calls per frame, and every one
//     of them costs a full trip round the pair loop's top of iteration (pc-oob test, fused record
//     load, the pending/ebit gate, the run-length load, the budget clamp, the MIN_RUN test, the
//     G500 tail lookup, the noinline 10-argument call, the next-pc fold, the cycle update) before
//     the first pair of the next block executes. Nobody has priced ONE such trip, so nobody knows
//     what a lever that CHAINS blocks (executes the next run without returning to run()) could be
//     worth. `DC2_G419_AB=blksplit` prices it directly: the candidate arm HALVES every block whose
//     budget-clamped length is >= 4, which leaves the executed pairs, their order and the coverage
//     bit-identical and adds exactly ONE extra round trip per split block. Frame delta divided by
//     the census's `split/f` IS the price of a round trip. It is an INFLATION probe, so it carries
//     none of G492's producer-edge deletion hazard: nothing downstream can derive different work
//     from it, and `[G500:inv] pairs/run` + `cover` are the per-arm invariant.
//
//  2. THE REMAINDER. The roadmap's task 2 — the ~9.4% of pairs that are neither block-eligible nor
//     control transfers — has been "unpriced because nobody has counted it" for two phases. A
//     STATIC slot tally in g490RebuildRunLengths would not answer it (the hot loop's slots execute
//     orders of magnitude more often than cold ones), so this census counts the reason DYNAMICALLY,
//     once per pair the PAIR LOOP executes, in the order g490BlockPairEligible tests them.
//
// Both halves are split by KICK KIND (G501 §3b: resume kicks are 29% of kicks but 58% of all pairs
// and their pairs cost 25% less), which is the roadmap's task 3.
//
// Census: DC2_G502_CENSUS=1 -> `[G502:blk]` / `[G502:excl]`. Never a timing arm — it puts one
//         out-of-line call on every plain pair.
// Probe:  DC2_G419_AB=blksplit (paired, hold 16), or DC2_G502_SPLIT=1 standalone.
// G502 §5 — WHAT THE CENSUS AND THE PRICE TOGETHER SELECTED: `blktail0`.
//
// `[G502:excl]` splits the remainder that has been called "the ~9.4% G139-hazard / short-run
// bucket" for two phases. It is not that. Weighted by each population's share of ALL pairs
// (geometry 41%, resume 59%):
//     branch   2.10%   control transfers the pair loop still executes
//     pending  2.05%   their delay slots
//     short    1.43%   an eligible run of exactly ONE pair
//     G139 haz 0.49%   <- a FIFTH of what the framing implied
//     eibit    0.32%   prestall 0.06%   ebit 0.005%   budget 0.00%
// (plus lknone 2.08% and flaglow 2.54%, both already priced and both NO-GO.)
//
// G500 admits a control transfer as a block TAIL only when the run before it already reached
// MIN_RUN, so `branch` + `pending` = **4.15% of all pairs, ~67 k/frame**, are exactly the
// population G500 measured at **7.0 ns/pair** (its −1.464 ms/f over 208.6 k tail pairs/frame) and
// they still pay the full per-pair envelope. `blktail0` admits a block on `count + tail` instead of
// on `count`, so a lone control transfer — with no eligible pair in front of it at all — forms a
// block of its own and swallows its delay slot. Ceiling: ~0.35 ms/f gross against ~34 k extra
// round trips at 1.89 ns = 0.064 ms/f, i.e. **~0.29 ms/f net**. Nothing else on the board prices
// above the floor.
//
// EXACTNESS: none. There is no new code in the block body and no new predicate — `g500TailEligible`
// already decides admissibility, `g490BlockBody` already runs `for (j = 0; j < count; ++j)` (which
// is simply empty when count is 0) and the caller's branch-outcome fold already handles
// `g500BPc == currentPc`. The oracle that verified G500's tails verifies these unchanged.
//
// Arm:    DC2_G419_AB=blktail0 (paired, hold 16)
// Kill:   DC2_G502_NO_BLKTAIL0=1
extern int g502BlkSplitArm();
extern int g502BlkChainArm();
extern int g502BlkTail0Arm();

// G504: the ring-cursor arm (DC2_G419_AB=histcur, rollback DC2_G504_NO_HISTCUR=1).
extern int g504HistCurArm();

// G506: the segmented Q/P-advance arm (DC2_G419_AB=qpseg, rollback DC2_G506_NO_QPSEG=1).
extern int g506QpSegArm();

// G499 §2 REFUTED and COMPILED OUT (`DC2_G499_QPLOCAL 0`). Four paired A/Bs on the same binary,
// both arms confirmed built on every run (`[G499:blk] arm=1` and `arm=2`), read
// **-0.050 / +0.191 / -0.057 / -0.031 ms/f: mean +0.013, signs disagree, |t| <= 1.14, blocked
// -0.010 / +0.186 / -0.059 / -0.017.** That is law 14's neutral, not a small win. The two memory
// operands it deletes per pair are L1-resident and the branch on them is perfectly predicted, so
// the out-of-order engine was already hiding them; the two extra loop-live ints are not free even
// in a leaf function with its own register file. **This is the SIXTH confirmation of the
// G427/G442/G479/G483/G485/G486 live-value law, and the first one taken OUTSIDE run()'s pair
// loop** — which is what makes it worth keeping: G479's `QPREG` refutation could be dismissed as a
// property of a loop already at ~1.2 IPC with 36 spills, and this one cannot.
//
// The scaffolding is kept (the template parameter, the arm, the flags) so ONE edit re-instantiates
// it, exactly like DC2_G485_HAZREG / DC2_G489_PCSKIP. Set the #define to 1 to re-arm; do not do so
// until something has changed what brackets those two counters.
#ifndef DC2_G499_QPLOCAL
#define DC2_G499_QPLOCAL 0
#endif

// G500: the control-transfer tail body. Built (a fourth instantiation would be a THIRD live one, so
// `qpblock` stays compiled out beside it — G499 §5c measured an identical source at -0.145 on a link
// carrying one extra 4.9 KB instantiation and -0.515 on the shipped one).
#ifndef DC2_G500_BLKBRANCH
#define DC2_G500_BLKBRANCH 1
#endif

// G502 §4 — THE CHAINED BLOCK BODY IS PRICED OUT AND COMPILED OUT, BY MEASUREMENT.
//
// `[G502:blk]` says 44.30% (geo) / 40.86% (res) of blocks are immediately followed by another one,
// i.e. ~66.8 k of the ~158 k block round trips per frame buy nothing. The `blksplit` ceiling probe
// then priced ONE round trip: **+0.360 ms/f mean over four paired runs, sign 4/4, for 190,653 extra
// round trips per frame ⇒ 1.89 ns each** — about seven host cycles for run()'s whole top of
// iteration PLUS a noinline 10-argument call. So the chain's ceiling is
// **66,800 × 1.89 ns = 0.126 ms/f**, and the chain does not even delete a whole round trip (it
// still evaluates the next-pc fold, the run-length load and the tail lookup inside the leaf), so
// the realistic figure is under 0.09. G501's own `blkpin` read −0.167 and was called "AT the
// instrument floor". **Below the floor: do not build it.**
//
// The scaffolding stays — the template parameter, the chain step, the wrapper, the arm accessor and
// the lever id — so ONE edit re-arms it, exactly like DC2_G479_QPREG / DC2_G485_HAZREG /
// DC2_G489_PCSKIP / DC2_G499_QPLOCAL. Do not set this to 1 until something has changed what a block
// round trip costs.
#ifndef DC2_G502_BLKCHAIN
#define DC2_G502_BLKCHAIN 0
#endif

// G504 — THE G430 RING CURSOR AS A BLOCK LOCAL.
//
// The first instruction-level attribution of this thread in the whole arc (G504 §3: `[G483:rawpc]`
// resolved against a linker map this phase made every link emit) puts the SINGLE HOTTEST
// instruction of the VU1 worker inside `g430PushPending`, and the push as a whole at ~9.2% of the
// thread. The mechanism is not instruction count — it is that consecutive pushes are chained
// through MEMORY (`H.head` is loaded, four stores are indexed off it, and it is stored back), so
// each push waits on the previous one's store to forward. Full argument and the exactness proof at
// `g430PushPendingAt` in vu1_g421_fast_upper.inc.
//
// G504 §4d — REFUTED over four paired runs, and COMPILED OUT. See the fix log for the numbers.
//
// The compiler did most of what was asked: `dumpbin` on the candidate shows the `hb` reload and the
// `H.head` load GONE, the ring index carried in `r15`, and the four store addresses no longer
// waiting on a load. It then **mirrored the cursor to a stack slot anyway** (`mov ecx,[rbp-0x60]` /
// `mov [rbp-0x60],edx` on every push), computing the wrap twice — so the store→load round trip
// survived, relocated from the TLS block to the stack frame, and the leaf's frame grew from
// `sub rsp,0x70` to `sub rsp,0x80`.
//
// SEVENTH confirmation of the live-value law (G427/G442/G479/G483/G485/G486), and the first one
// against a mechanism predicted to ESCAPE it: a loop-carried store→load dependence is the one class
// an out-of-order core cannot hide, and G485's `widevf0` — the only previous lever of that class
// here — won. It does not generalise. In a leaf that already spills six loop-invariant values per
// pair because it CALLS `g422FastLower`, taking one value out of memory only moves the spill.
//
// The scaffolding stays — the template parameter, `g430PushPendingAt`, `g504ExecuteBlockTailCur`,
// arm 6 and the lever id — so ONE edit re-arms it, exactly like DC2_G499_QPLOCAL / DC2_G502_BLKCHAIN
// / DC2_G502_BLKTAIL0. Do not set this to 1 again until something has changed what forces the spill
// (i.e. until the block stops calling out of line).
#ifndef DC2_G504_HISTCUR
#define DC2_G504_HISTCUR 0
#endif

// G504: the ring-cursor ORACLE, compiled out by default and re-armed with ONE edit — the
// `DC2_G490_VERIFY_BLOCK` convention, not a runtime flag, and for a measured reason. Holding the
// entry cursor live across the whole block for a cold comparison is an extra LIVE VALUE in the one
// loop this arc has proved binds on exactly that (G483): with the check compiled in, MSVC grew the
// leaf's frame from `sub rsp,0x70` to `sub rsp,0xa0` and kept the cursor in BOTH a register and a
// stack slot. It has already answered — **zero `[G504:verify-BAD]` over 513,437,981 block runs and
// 4.7 G pairs**, with `[G490:block] cover=88.99% avg=9.15` unchanged — so the shipped body carries
// none of it. Set to 1 (and rebuild) to re-verify after any change to what a block may execute.
#ifndef DC2_G504_VERIFY_CUR
#define DC2_G504_VERIFY_CUR 0
#endif

// G506 — THE Q/P DELAY ADVANCE, HOISTED OUT OF THE PER-PAIR LOOP BY SEGMENTATION.
//
// WHAT IS BEING REMOVED. The shipped body (`<1,0,1,1,0,0>`, i.e. G499QpLocal FALSE) runs this on
// EVERY pair, through the two POINTER parameters:
//
//     if (*qDelay > 0 && --(*qDelay) == 0) st.q = s_vuQPending;
//     if (*pDelay > 0 && --(*pDelay) == 0) st.p = s_vuPPending;
//
// `tools/g504_disasm.py` on the shipped leaf shows what that costs at the top of the loop
// (+0xc0..+0xf0): the `qDelay` pointer is RELOADED from its stack home on every iteration
// (`mov r8, qword ptr [rbp+0x68]`, because r8 is needed for the fused base first), then a load, a
// test, a branch, a decrement, a store and a second branch — twice. Two read-modify-writes to the
// same two words on every one of ~1.45 M block pairs per frame, plus a register the loop body then
// does not have.
//
// WHY IT IS PROVABLY DEAD WORK INSIDE A BLOCK. Both counters are ARMED only by scalar-pipe lowers:
// `s_vuQDelay` by DIV / SQRT / RSQRT (7/7/13) and WAITQ, in `execLower`; `s_vuPDelay` by
// `g239_stage_p` (the eight EFU producers) and `g239_wait_p` (WAITP), and by nothing else in the
// runtime — the complete writer set is enumerable and was enumerated (`s_vuQDelay =` / `s_vuPDelay =`
// have exactly these call sites). `g426LowerIsScalarPipe` marks precisely that opcode set
// (0x38-0x3a, 0x3b, 0x70-0x73, 0x76, 0x78-0x7a, 0x7b) as `G426_PF_PRESTALL`, and block eligibility
// REQUIRES that bit clear. No upper slot can reach either counter. Therefore, inside a block, both
// counters can only COUNT DOWN — the sequence of values a pair sees is a pure function of the entry
// value and the pair index, and the only observable effect is the single publication of `st.q` /
// `st.p` at the pair where the counter reaches zero.
//
// THE TRANSFORMATION. Split the pair run at those (at most two) publication points instead of
// testing for them per pair. Each segment executes with NO counter code at all; the publication
// happens once, between segments. The pair body appears ONCE in the source, so this adds an outer
// `while` and no second copy of the 13 KB leaf. A counter with value v at pair j publishes before
// pair j+v-1, which is exactly what the loop below computes.
//
// It also subsumes G499's `qpblock` (arm 2, refuted at +0.013 and compiled out): the counters must
// live in block locals for the segmentation to be expressible at all. That refutation is not
// evidence against this one — `qpblock` kept the per-pair decrement and only moved WHERE it read
// from, which is the live-value law's own subject. This deletes the per-pair work outright.
//
// Arm:    DC2_G419_AB=qpseg (paired, hold 16 — thread-level code shape). Control arm is 3, the
//         G500/G501 shipped body, so both arms execute the identical pairs in the identical blocks.
// Kill:   DC2_G506_NO_QPSEG=1
// Oracle: DC2_G506_VERIFY=1 — caller-side, once per BLOCK: after the block returns, `*qDelay` and
//         `*pDelay` must equal `max(entry - pairsConsumed, 0)`. A lower that re-armed a counter
//         inside the block would leave 7, 13 or 18 there instead, and an arithmetic error in the
//         segmentation would leave anything else. Costs nothing on the default path (one memoised
//         bool tested once per block) and nothing inside the leaf.
#ifndef DC2_G506_QPSEG
#define DC2_G506_QPSEG 1
#endif

// G502 §6 — `blktail0` IS REFUTED, over EIGHT consecutive paired runs on two links.
//
// The lever: admit a block on `count + tail` instead of on `count`, so a control transfer with
// fewer than MIN_RUN eligible pairs in front of it forms a block of its own and swallows its delay
// slot. §2 prices that population at 4.15% of all pairs and G500 measured exactly this population
// at 7.0 ns/pair, so it was pre-priced at −0.35 ms/f. It measured
// **+0.121 (4/4 slower) on the first link and +0.082 (4/4 slower) on the second** — the second
// carrying the pre-joined `g502TailAt` sidecar, which cut the penalty by 0.039 and did not flip it.
// Coverage did what it was supposed to (89.00% → 93.31%, +70 k pairs/frame off the pair loop) and
// the frame still got worse.
//
// WHY, and this is the durable half. **A block does not save the pair-loop iteration it is ENTERED
// from.** Moving L pairs into a NEW block saves `L × (per-pair envelope)` and costs one round trip;
// moving them into an EXISTING one costs nothing. G500's 7.0 ns/pair was measured on tail pairs
// that joined blocks **already being entered** — it never included an entry. Here every 2-pair
// block is a new entry, so the sum is `2 × envelope − 1.89 ns`, and the measurement says that is
// **≤ 0**: a branch pair executed by the pair loop costs about what one executed inside a block
// costs. Same law as G500 §8 (an average marginal rate does not price a selected population),
// firing in the pessimistic direction for the first time.
//
// Scaffolding kept, compiled out — DC2_G479_QPREG / DC2_G485_HAZREG / DC2_G489_PCSKIP convention.
#ifndef DC2_G502_BLKTAIL0
#define DC2_G502_BLKTAIL0 0
#endif

// G502 §3 — the round-trip ceiling probe has ANSWERED (1.89 ns per block round trip) and is
// compiled out with it: a default-off lever still costs the default path a test on every hook
// entry (Aggressive Performance Policy). Re-arm with one edit when something changes what a block
// entry costs.
#ifndef DC2_G502_SPLIT_PROBE
#define DC2_G502_SPLIT_PROBE 0
#endif

namespace
{
    // Minimum run length worth the call boundary. 2 mirrors G475 so the two are comparable.
    constexpr uint32_t G490_MIN_RUN = 2u;

    bool g490BlockRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blockrun") == 0;
        }();
        if (s_paired)
            return g490BlockRunArm() > 0;
        // PROMOTED default-on (G490): mean -0.560 ms/f raw / -0.523 blocked over four paired
        // A/Bs, sign 4/4, blocked t -2.0..-4.8, both arms confirmed built on every run.
        // Rollback: DC2_G490_NO_BLOCK=1 restores the G485 promoted copy exactly.
        static const bool s_on = !f50_12_vu_env_flag("DC2_G490_NO_BLOCK");
        return s_on;
    }

    bool g490VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_VERIFY");
        return s_on;
    }

#if DC2_G506_QPSEG
    // G506 oracle, caller side. Memoised like every other arm accessor (G500 §4: an uncached
    // getenv on a path reached ~158 k times per frame is 8 ms/f).
    bool g506VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G506_VERIFY");
        return s_on;
    }

    // The premise, checked directly: inside a block both scalar-pipe counters can only COUNT DOWN,
    // so after `used` pairs each must hold `max(entry - used, 0)`. Reports both arms, so a
    // disagreement can be attributed to the segmentation (arm 7) or to the premise itself (arm 3,
    // which runs the untouched per-pair form and must satisfy the same identity).
    void g506VerifyBlockQp(int qEntry, int pEntry, uint32_t used, int qNow, int pNow, int arm)
    {
        const int wantQ = static_cast<int>(used) >= qEntry ? 0 : qEntry - static_cast<int>(used);
        const int wantP = static_cast<int>(used) >= pEntry ? 0 : pEntry - static_cast<int>(used);
        static std::atomic<uint64_t> s_blocks{0};
        s_blocks.fetch_add(1u, std::memory_order_relaxed);
        if (qNow == wantQ && pNow == wantP)
            return;
        static std::atomic<uint32_t> s_bad{0};
        const uint32_t n = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 32u)
            std::fprintf(stderr,
                         "[G506:verify-BAD] arm=%d used=%u q(entry=%d now=%d want=%d) "
                         "p(entry=%d now=%d want=%d) n=%u blocks=%llu\n",
                         arm, used, qEntry, qNow, wantQ, pEntry, pNow, wantP, n,
                         static_cast<unsigned long long>(
                             s_blocks.load(std::memory_order_relaxed)));
    }
#endif

    bool g490StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_STAT");
        return s_on;
    }

    // G499: which block body run() calls. ONE value, read ONCE per run() (G479's rule — a
    // microprogram must never change dispatch shape mid-execution), so the pair loop carries a
    // single live int instead of one bool per mechanism.
    //   0 = pre-G499 body            (DC2_G499_NO_BLKLOWER=1, and the `blklower` control arm)
    //   1 = + G480/G481 lower dispatch (the `blklower` candidate / the `blkbranch` CONTROL)
    //   2 = + Q/P counters as block locals (REFUTED G499 §5b, compiled out)
    //   3 = + G500 control-transfer TAIL (the `blkbranch` candidate). G501's pinned addressing
    //       is UNCONDITIONAL inside this body, not a fourth arm — it deletes code `g443Fixed`
    //       proves unreachable, so it has no control arm to be compared against.
    //   4 = reserved for the `blksplit` CEILING PROBE, which changes no body at all (it only
    //       shortens the run the pair loop hands to arm 3) and therefore has no body of its own.
    //   5 = + G502 CHAIN (the `blkchain` candidate). Control arm is 3, the G500 shipped body.
    //   6 = + G504 HISTORY CURSOR (the `histcur` candidate): arm 3's body with the G430 ring cursor
    //       carried in a block local instead of in `H.head`. Control arm is 3, so the two arms
    //       execute the identical pairs in the identical blocks and differ only in where four
    //       stores get their index from.
    int g499BlkArmRun()
    {
        static const int s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            if (ab == nullptr)
                return 0;
            if (std::strcmp(ab, "blklower") == 0)
                return 1;
            if (std::strcmp(ab, "qpblock") == 0)
                return 2;
            if (std::strcmp(ab, "blkbranch") == 0)
                return 3;
            if (std::strcmp(ab, "blkchain") == 0)
                return 5;
            if (std::strcmp(ab, "histcur") == 0)
                return 6;
            if (std::strcmp(ab, "qpseg") == 0)
                return 7;
            return 0;
        }();
        if (s_paired == 1)
            return g499BlkLowerArm() > 0 ? 1 : 0;   // 0 vs 1: prices the lower dispatch alone
#if DC2_G499_QPLOCAL
        if (s_paired == 2)
            return g499QpBlockArm() > 0 ? 2 : 1;    // 1 vs 2: prices the Q/P residency alone
#endif
#if DC2_G500_BLKBRANCH
        // 1 vs 3: prices the control-transfer tail alone. The control arm is the G499 shipped body,
        // so nothing but the tail differs.
        if (s_paired == 3)
            return g500BlkBranchArm() > 0 ? 3 : 1;
#endif
#if DC2_G502_BLKCHAIN
        // 3 vs 5: prices the chain alone. The control arm is the G500 shipped body, and the two
        // arms execute the identical pairs in the identical blocks — only the return to run()
        // between two consecutive blocks differs, which `[G500:inv]`'s `calls=` column isolates.
        if (s_paired == 5)
            return g502BlkChainArm() > 0 ? 5 : 3;
#endif
#if DC2_G504_HISTCUR
        // 3 vs 6: prices the ring cursor's residency alone. Control is the G500/G501 shipped body.
        if (s_paired == 6)
            return g504HistCurArm() > 0 ? 6 : 3;
#endif
#if DC2_G506_QPSEG
        // 3 vs 7: prices the per-pair Q/P advance alone. Control is the G500/G501 shipped body, so
        // both arms execute the identical pairs in the identical blocks and differ only in whether
        // two counters are tested once per PAIR or once per SEGMENT.
        if (s_paired == 7)
            return g506QpSegArm() > 0 ? 7 : 3;
#endif
        // G500 §4 — THE ROLLBACK FLAGS ARE READ ONCE, NOT ONCE PER run().
        //
        // `f50_12_vu_env_flag` is an UNCACHED `std::getenv` (vu1_helpers_and_tables.inc), and this
        // function is called once per run() — ~1,236 times per FRAME on this route. G499 put one
        // such call on the default path here and G500 briefly added a second; every other arm
        // accessor in this file (g490BlockRequested, g490StatOn, g499StatOn, …) already memoises in
        // a function-local `static const`, which is why nobody had noticed.
        //
        // It also made the two ESTIMATORS DISAGREE IN SIGN. The paired A/B returns at the
        // `s_paired` branches above and never reaches these calls, so it prices the block bodies
        // cleanly; a lean run pays them, and a getenv MISS scans the whole environment block while
        // a HIT stops at the match — so `DC2_G500_NO_BLKBRANCH=1` made its own rollback arm cheaper
        // than the default it was supposed to be compared against. Six interleaved lean runs read
        // the candidate 2.1 ms/f SLOWER while six paired runs read it 1.45 ms/f FASTER, purely
        // because of that asymmetry. Memoising removes both the cost and the bias.
        static const int s_default = [] {
            if (f50_12_vu_env_flag("DC2_G499_NO_BLKLOWER"))
                return 0;
#if DC2_G506_QPSEG
            // G506: the segmented Q/P advance rides on the G500 tail body, so its rollback falls
            // back to arm 3 exactly as `histcur`'s did.
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G506_NO_QPSEG"))
                return 7;
#endif
#if DC2_G504_HISTCUR
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G504_NO_HISTCUR"))
                return 6;
#endif
#if DC2_G502_BLKCHAIN
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G502_NO_BLKCHAIN"))
                return 5;
#endif
#if DC2_G500_BLKBRANCH
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH"))
                return 3;
#endif
#if DC2_G499_QPLOCAL
            return f50_12_vu_env_flag("DC2_G499_NO_QPLOCAL") ? 1 : 2;
#else
            return 1; // arm 2 is compiled out; see the DC2_G499_QPLOCAL note at the head of the file
#endif
        }();
        return s_default;
    }

    bool g499StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G499_STAT");
        return s_on;
    }

    // G504: the ring-cursor oracle. Memoised in a function-local `static const` (G500's law — an
    // uncached getenv reachable from the block leaf is the exact shape that cost ~8 ms/f).
    bool g504VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G504_VERIFY");
        return s_on;
    }

    // ---------------------------------------------------------------------------------------
    // Eligibility + the per-slot maximal run length, maintained beside G410's precisely
    // invalidated pair cache (same contract as G475's table: rebuilt on a full cache build and on
    // every MPG invalidation, so it can never describe a stale encoding).
    // ---------------------------------------------------------------------------------------
    bool g490BlockPairEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;                                   // E-bit or LOI
        if (pair.lowerKind == G422_LK_NONE)
            return false;                                   // needs the legacy execLower
        if (pair.lowerKind >= G422_LK_B && pair.lowerKind <= G422_LK_IBGEZ)
            return false;                                   // branch / jump: writes pc
        if (pair.lowerKind >= G422_LK_FIRST_FLAG)
            return false;                                   // reads the MAC/STATUS line
        if ((pair.g425Flags &
             (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u)
            return false;                                   // per-pair machinery is live
        if (pair.upperKind == 0x6fu)
            return true;                                    // VNOP: the loop skips the upper
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    // G500: the CONTROL-TRANSFER TAIL predicate. Identical to g490BlockPairEligible except that the
    // lower is required to BE a branch/jump instead of required not to be one. Every other clause is
    // the same, and for the same reason:
    //   * `pairFlags == 0`   -> no E-bit (no latch, no drain) and no I-bit.
    //   * `!G426_PF_PRESTALL` / `!G426_PF_HAZ` / `!G427_PF_FLAGREAD` -> the three per-pair blocks
    //     the pair loop runs around the slots are provably inert for this pair. (A branch lower can
    //     never set PRESTALL, and reads no flag line, but HAZ is a pure function of the two
    //     encodings and is honoured rather than reasoned away — same conservatism as G489's I-bit.)
    //   * upper is VNOP (skipped) or `cls != 0` (`g421FastUpper` executes it).
    // What the tail pair additionally needs, and what the block body supplies, is the architectural
    // PC: `G481_LOWER_BODY_B/BAL/JR/IBEQ/...` compute their target from `st.pc`, and BAL/JALR write
    // `(st.pc + 16) / 8` into VI. The block writes `st.pc` exactly once, immediately before the tail
    // lower, which is the same value the pair loop would have had live there.
    bool g500BranchTailEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;                                   // E-bit or LOI
        if (pair.lowerKind < G422_LK_B || pair.lowerKind > G422_LK_IBGEZ)
            return false;                                   // not a control transfer
        if ((pair.g425Flags &
             (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u)
            return false;                                   // per-pair machinery is live
        if (pair.upperKind == 0x6fu)
            return true;                                    // VNOP: the loop skips the upper
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    void g490RebuildRunLengths()
    {
        uint32_t run = 0u;
        for (size_t i = s_g410Cache.uppers.size(); i-- > 0u;)
        {
            if (g490BlockPairEligible(s_g410Cache.uppers[i]))
            {
                if (run < 255u)
                    ++run;
                s_g410Cache.g490RunLen[i] = static_cast<uint8_t>(run);
            }
            else
            {
                run = 0u;
                s_g410Cache.g490RunLen[i] = 0u;
            }
            // G500: 0 / 1 / 2 extra pairs a block may execute as a control-transfer TAIL at this
            // slot. `g490RunLen[i + 1]` has already been written by this same backward pass, and it
            // is non-zero exactly when slot i+1 is g490BlockPairEligible — which is the delay-slot
            // condition, so no second eligibility call is needed.
            uint8_t tail = 0u;
            if (g500BranchTailEligible(s_g410Cache.uppers[i]))
            {
                tail = 1u;
                if (i + 1u < s_g410Cache.g490RunLen.size() &&
                    s_g410Cache.g490RunLen[i + 1u] != 0u)
                    tail = 2u;
            }
            s_g410Cache.g500Tail[i] = tail;
            // G502: pre-join the tail the block STARTING at this slot would find, so the hook is a
            // single byte load at the slot it is already indexing. `i + g490RunLen[i] > i`, so the
            // backward pass has already written that entry.
            const size_t g502At = i + static_cast<size_t>(s_g410Cache.g490RunLen[i]);
            s_g410Cache.g502TailAt[i] =
                (g502At < s_g410Cache.g500Tail.size()) ? s_g410Cache.g500Tail[g502At] : 0u;
        }
    }

    // ---------------------------------------------------------------------------------------
    // Stats. Out of line and behind a cached bool, so the promoted copy carries one test and a
    // never-taken call (G421 law 17: never inline a census into this loop).
    // ---------------------------------------------------------------------------------------
    thread_local uint64_t s_g490Runs = 0u;
    thread_local uint64_t s_g490Pairs = 0u;
    thread_local uint64_t s_g490TotalPairs = 0u;
    thread_local uint64_t s_g490LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490NoteBlock(uint32_t count)
    {
        ++s_g490Runs;
        s_g490Pairs += count;
        if (s_g490Pairs - s_g490LastReport >= 50000000u)
        {
            s_g490LastReport = s_g490Pairs;
            std::fprintf(stderr,
                         "[G490:block] runs=%llu pairs=%llu avg=%.2f cover=%.2f%%\n",
                         static_cast<unsigned long long>(s_g490Runs),
                         static_cast<unsigned long long>(s_g490Pairs),
                         static_cast<double>(s_g490Pairs) /
                             static_cast<double>(s_g490Runs ? s_g490Runs : 1u),
                         s_g490TotalPairs
                             ? 100.0 * static_cast<double>(s_g490Pairs) /
                                   static_cast<double>(s_g490TotalPairs)
                             : 0.0);
        }
    }

    // G502: the same counter for a CHAIN, which ran `runs` blocks in one entry. Keeps `avg=` and
    // `cover=` meaning exactly what they meant before the chain existed.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490NoteBlocks(uint32_t runs, uint32_t count)
    {
        s_g490Runs += runs;
        s_g490Pairs += count;
        if (s_g490Pairs - s_g490LastReport >= 50000000u)
        {
            s_g490LastReport = s_g490Pairs;
            std::fprintf(stderr,
                         "[G490:block] runs=%llu pairs=%llu avg=%.2f cover=%.2f%%\n",
                         static_cast<unsigned long long>(s_g490Runs),
                         static_cast<unsigned long long>(s_g490Pairs),
                         static_cast<double>(s_g490Pairs) /
                             static_cast<double>(s_g490Runs ? s_g490Runs : 1u),
                         s_g490TotalPairs
                             ? 100.0 * static_cast<double>(s_g490Pairs) /
                                   static_cast<double>(s_g490TotalPairs)
                             : 0.0);
        }
    }

    // Called ONCE per run() (~1,230/frame), never per pair, so `cover=` is exact without putting
    // a counter in the loop.
    void g490NoteTotalPairs(uint32_t cycles)
    {
        s_g490TotalPairs += cycles;
    }

    // -------------------------------------------------------------------------------------------
    // G500: the PER-ARM WORKLOAD INVARIANT. A deletion/relocation lever must prove that both arms
    // did the same work (G492's -24.2 ms/f artefact was a candidate arm building 35 draws against
    // 829), and a coverage lever must prove the coverage actually moved. Both arms of `blkbranch`
    // execute the identical microprogram, so kicks/f, pairs/kick and blocks/f must be INVARIANT
    // while tailPairs and cover must differ — that pair of statements is the whole check.
    //
    // Off by default (DC2_G500_STAT=1), out of line, behind a bool cached once, and called once per
    // BLOCK and once per run() — never per pair (G421 law 17).
    // -------------------------------------------------------------------------------------------
    bool g500StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G500_STAT");
        return s_on;
    }

    struct G500Inv
    {
        uint64_t runs = 0u;
        uint64_t totalPairs = 0u;
        uint64_t blocks = 0u;
        uint64_t plainPairs = 0u;
        uint64_t tail1 = 0u;
        uint64_t tail2 = 0u;
        uint64_t taken = 0u;
        // G502: how many times run() ENTERED the block executor. Identical to `blocks` on every arm
        // before this phase; on the chain arm it is `blocks` minus the round trips the chain
        // deleted, which is the ONE column the two arms of `blkchain` are allowed to differ in.
        uint64_t calls = 0u;
        uint64_t lastReport = 0u;
    };
    // G501: indexed by the ACTUAL arm value, not by `arm != 0`. `blkbranch` alternates arms 1
    // and 3, but `blkpin` alternates 3 and 4 — with a two-bucket array keyed on `arm >= 3` both of
    // its arms collapsed into the same bucket and the per-arm invariant, whose entire job is to
    // prove the two arms did the SAME work, could not have shown a difference. Same defect class
    // as g499ReportArmed's `s_once[3]`, and the same fix.
    // G502: eight slots, so arm 5 (the chain candidate) has one of its own. Same reason G501 went
    // from 3 to 5 — a per-arm instrument that folds two arms into one bucket cannot show a
    // difference, which is the entire job of a workload invariant.
    thread_local G500Inv s_g500Inv[8];

    inline int g500InvSlot(int arm) { return (arm < 0 || arm > 7) ? 0 : arm; }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteBlock(int arm, uint32_t plain, uint32_t tail, bool taken)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        ++v.blocks;
        ++v.calls;
        v.plainPairs += plain;
        if (tail == 1u)
            ++v.tail1;
        else if (tail == 2u)
            ++v.tail2;
        if (taken)
            ++v.taken;
    }

    // G502: the same invariant for ONE CHAIN — a single entry into the block executor that ran
    // `segments` blocks. `blocks`, `plainPairs`, `tail1`, `tail2` and `taken` accumulate exactly as
    // they would have without the chain (that is what makes them the invariant); only `calls` sees
    // one instead of `segments`.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteChain(int arm, uint32_t segments, uint32_t plain, uint32_t tail1,
                       uint32_t tail2, uint32_t taken)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        v.blocks += segments;
        ++v.calls;
        v.plainPairs += plain;
        v.tail1 += tail1;
        v.tail2 += tail2;
        v.taken += taken;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteRun(int arm, uint32_t cycles)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        ++v.runs;
        v.totalPairs += cycles;
        if (v.totalPairs - v.lastReport < 50000000u)
            return;
        v.lastReport = v.totalPairs;
        const double blocks = static_cast<double>(v.blocks ? v.blocks : 1u);
        const uint64_t tailPairs = v.tail1 + 2u * v.tail2;
        std::fprintf(stderr,
                     "[G500:inv] arm=%d runs=%llu pairs/run=%.1f blocks=%llu calls=%llu "
                     "pairs/blk=%.2f tail1=%llu tail2=%llu taken=%llu cover=%.2f%%\n",
                     g500InvSlot(arm),
                     static_cast<unsigned long long>(v.runs),
                     static_cast<double>(v.totalPairs) /
                         static_cast<double>(v.runs ? v.runs : 1u),
                     static_cast<unsigned long long>(v.blocks),
                     static_cast<unsigned long long>(v.calls),
                     (static_cast<double>(v.plainPairs) + static_cast<double>(tailPairs)) / blocks,
                     static_cast<unsigned long long>(v.tail1),
                     static_cast<unsigned long long>(v.tail2),
                     static_cast<unsigned long long>(v.taken),
                     v.totalPairs
                         ? 100.0 * (static_cast<double>(v.plainPairs) +
                                    static_cast<double>(tailPairs)) /
                               static_cast<double>(v.totalPairs)
                         : 0.0);
        std::fflush(stderr);
    }

    // -------------------------------------------------------------------------------------------
    // G502 - THE BLOCK-ENTRY CENSUS AND THE ROUND-TRIP CEILING PROBE.
    // -------------------------------------------------------------------------------------------
    bool g502CensusOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G502_CENSUS");
        return s_on;
    }

    // The split arm. Read ONCE per run() like g499BlkArmRun (G479's rule: a microprogram must not
    // change dispatch shape mid-execution), and memoised behind a function-local `static const` on
    // the default path — G500 §4's blind spot is exactly an arm accessor whose default path reaches
    // an uncached getenv.
#if DC2_G502_SPLIT_PROBE
    int g502SplitArmRun()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blksplit") == 0;
        }();
        if (s_paired)
            return g502BlkSplitArm() > 0 ? 1 : 0;
        static const int s_default = f50_12_vu_env_flag("DC2_G502_SPLIT") ? 1 : 0;
        return s_default;
    }
#endif // DC2_G502_SPLIT_PROBE

    // The tail-0 admission. Same contract: read once per run(), memoised on the default path.
#if DC2_G502_BLKTAIL0
    int g502Tail0ArmRun()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blktail0") == 0;
        }();
        if (s_paired)
            return g502BlkTail0Arm() > 0 ? 1 : 0;
        static const int s_default = f50_12_vu_env_flag("DC2_G502_NO_BLKTAIL0") ? 0 : 1;
        return s_default;
    }
#endif // DC2_G502_BLKTAIL0

    // Why a pair the PAIR LOOP executed was not in a block. Tested in g490BlockPairEligible's own
    // order, so the first matching reason is the one that actually decided.
    enum G502Excl : int
    {
        G502_X_PENDING = 0,   // branch delay slot: the hook is gated on pendingBranchTarget
        G502_X_EBIT,          // E-bit latched: the hook fails closed
        G502_X_BUDGET,        // eligible run present but maxCycles - cycle < MIN_RUN
        G502_X_SHORT,         // eligible, but the maximal run from here is 1 pair
        G502_X_EIBIT,         // pairFlags != 0 (E-bit or LOI)
        G502_X_LKNONE,        // lower needs the legacy execLower (DIV/XGKICK/...)
        G502_X_BRANCH,        // control transfer (G500 takes these as TAILS, not as run members)
        G502_X_FLAGLOW,       // flag lower (FCAND/FCOR/FMAND/FCGET)
        G502_X_PRESTALL,      // G239 scalar prestall
        G502_X_HAZ,           // G139 pair hazard
        G502_X_FLAGREAD,      // G427 MAC/STATUS consumer
        G502_X_UPPER0,        // upper class 0 (execUpper fallback)
        G502_X_COUNT
    };

    static const char *const kG502ExclName[G502_X_COUNT] = {
        "pending", "ebit", "budget", "short", "eibit", "lknone",
        "branch", "flaglow", "prestall", "haz", "flagread", "upper0"};

    struct G502Win
    {
        uint64_t blocks = 0u;       // blocks formed
        uint64_t blkPairs = 0u;     // plain + tail pairs executed inside them
        uint64_t chain = 0u;        // blocks whose SUCCESSOR would immediately form another block
        uint64_t extraTrips = 0u;   // EXTRA round trips the blksplit probe adds at this block
        uint64_t tail1 = 0u;
        uint64_t tail2 = 0u;
        uint64_t taken = 0u;
        uint64_t plain = 0u;        // pairs executed by the pair loop
        uint64_t excl[G502_X_COUNT] = {};
    };
    // [0] = geometry kick, [1] = resume kick. Never keyed on a BOOLEAN OF AN ARM (G501 §6) — this
    // index is the KICK KIND, which is what the split is about.
    thread_local G502Win s_g502Win[2];
    thread_local uint64_t s_g502Report = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502Report()
    {
        for (int k = 0; k < 2; ++k)
        {
            const G502Win &w = s_g502Win[k];
            const double blocks = static_cast<double>(w.blocks ? w.blocks : 1u);
            const uint64_t total = w.blkPairs + w.plain;
            std::fprintf(stderr,
                         "[G502:blk] kind=%s blocks=%llu pairs/blk=%.2f cover=%.2f%% "
                         "chain=%.2f%% split=%.2f%% tail1=%llu tail2=%llu taken=%.1f%% plain=%llu\n",
                         k ? "res" : "geo",
                         static_cast<unsigned long long>(w.blocks),
                         static_cast<double>(w.blkPairs) / blocks,
                         total ? 100.0 * static_cast<double>(w.blkPairs) /
                                     static_cast<double>(total) : 0.0,
                         100.0 * static_cast<double>(w.chain) / blocks,
                         100.0 * static_cast<double>(w.extraTrips) / blocks,
                         static_cast<unsigned long long>(w.tail1),
                         static_cast<unsigned long long>(w.tail2),
                         100.0 * static_cast<double>(w.taken) /
                             static_cast<double>((w.tail1 + w.tail2) ? (w.tail1 + w.tail2) : 1u),
                         static_cast<unsigned long long>(w.plain));
            const double pl = static_cast<double>(w.plain ? w.plain : 1u);
            const double tot = static_cast<double>(total ? total : 1u);
            std::fprintf(stderr, "[G502:excl] kind=%s", k ? "res" : "geo");
            for (int r = 0; r < G502_X_COUNT; ++r)
                std::fprintf(stderr, " %s=%.2f%%/%.2f%%", kG502ExclName[r],
                             100.0 * static_cast<double>(w.excl[r]) / pl,
                             100.0 * static_cast<double>(w.excl[r]) / tot);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    }

    // Once per BLOCK (~157 k/frame), out of line, behind a bool cached once — never per pair.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502NoteBlock(uint32_t plain, uint32_t tail, bool taken, uint32_t clamped,
                       uint32_t nextPc, uint32_t remaining, bool pending, uint32_t codeSize)
    {
        G502Win &w = s_g502Win[s_g502Resume ? 1 : 0];
        ++w.blocks;
        w.blkPairs += plain + tail;
        if (tail == 1u)
            ++w.tail1;
        else if (tail == 2u)
            ++w.tail2;
        if (taken)
            ++w.taken;
        // How many EXTRA round trips would the blksplit probe add here? The halving is applied
        // again to every remainder that still reaches 2*MIN_RUN (the pair loop cannot know it is
        // looking at a remainder), so a run of 9 becomes 5+2+2 — TWO extra trips, not one. Counted
        // exactly rather than closed-form, because this is the denominator the probe's frame delta
        // is divided by and a wrong denominator is a wrong price.
        for (uint32_t n = clamped; n >= 2u * G490_MIN_RUN; n -= (n + 1u) / 2u)
            ++w.extraTrips;
        // Would a CHAINING executor have kept going instead of returning to run()? Exactly the
        // conditions the pair loop's own hook re-tests one iteration later.
        if (!pending && remaining >= G490_MIN_RUN && nextPc + 8u <= codeSize)
        {
            const uint32_t avail =
                static_cast<uint32_t>(s_g410Cache.g490RunLen[nextPc >> 3u]);
            if (avail >= G490_MIN_RUN)
                ++w.chain;
        }
    }

    // Once per pair the PAIR LOOP executes (~11% of all pairs, ~178 k/frame). Out of line so the
    // promoted copy carries one test and a never-taken call (G421 law 17).
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502NotePlain(const G410CompiledUpper *p, uint32_t avail, uint32_t budget,
                       bool pending, bool ebit)
    {
        G502Win &w = s_g502Win[s_g502Resume ? 1 : 0];
        ++w.plain;
        int r;
        if (pending)
            r = G502_X_PENDING;
        else if (ebit)
            r = G502_X_EBIT;
        else if (avail >= G490_MIN_RUN)
            r = G502_X_BUDGET;               // an eligible run was there; the budget clamped it
        else if (avail != 0u)
            r = G502_X_SHORT;                // eligible, but a run of exactly one
        else if (p->pairFlags != 0u)
            r = G502_X_EIBIT;
        else if (p->lowerKind == G422_LK_NONE)
            r = G502_X_LKNONE;
        else if (p->lowerKind >= G422_LK_B && p->lowerKind <= G422_LK_IBGEZ)
            r = G502_X_BRANCH;
        else if (p->lowerKind >= G422_LK_FIRST_FLAG)
            r = G502_X_FLAGLOW;
        else if ((p->g425Flags & G426_PF_PRESTALL) != 0u)
            r = G502_X_PRESTALL;
        else if ((p->g425Flags & G426_PF_HAZ) != 0u)
            r = G502_X_HAZ;
        else if ((p->g425Flags & G427_PF_FLAGREAD) != 0u)
            r = G502_X_FLAGREAD;
        else
            r = G502_X_UPPER0;
        ++w.excl[r];
        if (++s_g502Report >= 5000000u)
        {
            s_g502Report = 0u;
            g502Report();
        }
    }

    // ---------------------------------------------------------------------------------------
    // Oracle. Re-derives, from the LIVE code words, every predicate the elision rests on. This is
    // the risk that is actually new in G490 (a wrong eligibility decode silently drops a hazard,
    // a prestall or a flag read); the arithmetic of the two slots is already covered by G421's
    // and G422's own shadow oracles, and the compact-loop SHAPE by G475's 40 M-pair state oracle.
    // ---------------------------------------------------------------------------------------
    std::atomic<uint64_t> s_g490Verified{0u};

    [[noreturn]] void g490VerifyFail(const char *what, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::fprintf(stderr,
                     "[G490:verify-BAD] %s pc=0x%x lower=%08x upper=%08x\n",
                     what, pc, lower, upper);
        std::fflush(stderr);
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490VerifyBlock(const uint8_t *vuCode, uint32_t firstIndex, uint32_t count)
    {
        for (uint32_t j = 0u; j < count; ++j)
        {
            const size_t index = static_cast<size_t>(firstIndex + j);
            const uint32_t pc = static_cast<uint32_t>(index * 8u);
            uint64_t live = 0u;
            std::memcpy(&live, vuCode + pc, sizeof(live));
            const G410CompiledUpper &p = s_g410Cache.uppers[index];
            if (live != s_g410Cache.sources[index] || live != s_g410Cache.fused[index].source)
                g490VerifyFail("stale-source", pc, (uint32_t)live, (uint32_t)(live >> 32));

            const uint32_t lower = static_cast<uint32_t>(live);
            const uint32_t upper = static_cast<uint32_t>(live >> 32);

            // E-bit / I-bit straight off the raw upper word.
            if (((upper >> 30u) & 1u) != 0u || ((upper >> 31u) & 1u) != 0u)
                g490VerifyFail("ei-bit", pc, lower, upper);
            // G239 scalar prestall.
            const uint8_t liveS2 =
                ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
            if (g426LowerIsScalarPipe(liveS2))
                g490VerifyFail("prestall", pc, lower, upper);
            // G427 MAC/STATUS consumer.
            if (g427LowerReadsFlags(lower))
                g490VerifyFail("flagread", pc, lower, upper);
            // G489: only the ten branch/jump lowers write pc.
            if (g489LowerWritesPc(lower))
                g490VerifyFail("writes-pc", pc, lower, upper);
            // G139 pair hazard, re-derived exactly as the G426 predecode verifier does.
            uint32_t liveIdx = 0xffffffffu;
            const uint8_t liveOp = static_cast<uint8_t>(upper & 0x3Fu);
            if (liveOp < 0x3Cu)
                liveIdx = (upper >> 6) & 0x1Fu;
            else
            {
                const uint8_t liveS2u = S2_FUNC(upper);
                if ((liveS2u >= 0x10u && liveS2u <= 0x17u) || liveS2u == 0x1Du)
                    liveIdx = (upper >> 16) & 0x1Fu;
            }
            if (liveIdx != 0xffffffffu &&
                ((((lower >> 11u) & 0x1Fu) == liveIdx) || (((lower >> 16u) & 0x1Fu) == liveIdx)))
                g490VerifyFail("pairhaz", pc, lower, upper);
            // And the cached flags this block's eligibility was actually taken from.
            if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u ||
                p.pairFlags != 0u || !g490BlockPairEligible(p))
                g490VerifyFail("cached-flags", pc, lower, upper);
        }
        const uint64_t old = s_g490Verified.fetch_add(count, std::memory_order_relaxed);
        if (old == 0u || old / 100000000u != (old + count) / 100000000u)
            std::fprintf(stderr, "[G490:verify] pairs=%llu bad=0\n",
                         static_cast<unsigned long long>(old + count));
    }

    // G500: the same oracle for the control-transfer TAIL. Everything g490VerifyBlock asserts,
    // except that the terminator's lower MUST be one of the ten pc-writers rather than must not be,
    // and the delay slot (when the tail is 2) is put through the plain check.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500VerifyTail(const uint8_t *vuCode, uint32_t branchIndex, uint32_t tailCount)
    {
        const uint32_t pc = branchIndex * 8u;
        uint64_t live = 0u;
        std::memcpy(&live, vuCode + pc, sizeof(live));
        const G410CompiledUpper &p = s_g410Cache.uppers[branchIndex];
        if (live != s_g410Cache.sources[branchIndex] ||
            live != s_g410Cache.fused[branchIndex].source)
            g490VerifyFail("tail-stale-source", pc, (uint32_t)live, (uint32_t)(live >> 32));

        const uint32_t lower = static_cast<uint32_t>(live);
        const uint32_t upper = static_cast<uint32_t>(live >> 32);
        if (((upper >> 30u) & 1u) != 0u || ((upper >> 31u) & 1u) != 0u)
            g490VerifyFail("tail-ei-bit", pc, lower, upper);
        const uint8_t liveS2 = ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
        if (g426LowerIsScalarPipe(liveS2))
            g490VerifyFail("tail-prestall", pc, lower, upper);
        if (g427LowerReadsFlags(lower))
            g490VerifyFail("tail-flagread", pc, lower, upper);
        // The terminator MUST be a pc writer — that is what makes it a tail rather than a plain
        // pair, and it is the one clause inverted against g490VerifyBlock.
        if (!g489LowerWritesPc(lower))
            g490VerifyFail("tail-not-branch", pc, lower, upper);
        uint32_t liveIdx = 0xffffffffu;
        const uint8_t liveOp = static_cast<uint8_t>(upper & 0x3Fu);
        if (liveOp < 0x3Cu)
            liveIdx = (upper >> 6) & 0x1Fu;
        else
        {
            const uint8_t liveS2u = S2_FUNC(upper);
            if ((liveS2u >= 0x10u && liveS2u <= 0x17u) || liveS2u == 0x1Du)
                liveIdx = (upper >> 16) & 0x1Fu;
        }
        if (liveIdx != 0xffffffffu &&
            ((((lower >> 11u) & 0x1Fu) == liveIdx) || (((lower >> 16u) & 0x1Fu) == liveIdx)))
            g490VerifyFail("tail-pairhaz", pc, lower, upper);
        if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u ||
            p.pairFlags != 0u || !g500BranchTailEligible(p))
            g490VerifyFail("tail-cached-flags", pc, lower, upper);
        if (tailCount == 2u)
            g490VerifyBlock(vuCode, branchIndex + 1u, 1u);
    }

    // ---------------------------------------------------------------------------------------
    // THE BLOCK. `noinline` on purpose: G481 measured this loop's budget binding on `run()`'s
    // instruction FOOTPRINT, so a second inlined copy of the SIMD upper + fast lower inside
    // run() would be paid by every pair to help a subset. Non-volatile registers survive the
    // call, and the hook sits at the TOP of the iteration where no pair-local value is live yet,
    // so the boundary costs the argument setup and nothing that the pair body holds.
    //
    // Order is the pair loop's order, statement for statement, for the eligible class:
    //   Q advance -> P advance -> upper -> lower -> VF0/VI0 re-pin.
    //
    // G499: ONE body, three instantiations. `G499FastLower` and `G499QpLocal` are compile-time
    // constants, so the control arm's text is byte-identical to what it was before this phase and
    // each candidate's extra code exists only in that candidate — an in-body runtime selector
    // would sit on BOTH arms and bias the gate (G436).
    //
    // G499 §2 — THE Q/P COUNTERS AS BLOCK LOCALS (`G499QpLocal`).
    //
    // Both counters are read-modify-written through POINTERS on every pair, and the loop calls
    // `g422FastLower` in between — which stores into `vuData` (SQ/SQI/SQD). MSVC cannot prove
    // `int *qDelay` does not alias that array, so it must RELOAD both from memory at the top of
    // every iteration, and that load sits in the dependency chain of the iteration's first branch.
    //
    // Hoisting them is EXACT here in a way it is not in the pair loop. Block eligibility excludes
    // `G426_PF_PRESTALL` (every FDIV/EFU producer and WAITQ/WAITP) and every lower `g422FastLower`
    // does not cover, and G479 established that the ONLY writer of these two counters outside the
    // loop body is `execLower`, which a block can never reach. So inside a block the counters can
    // only COUNT DOWN, never be re-armed: a local copy written back once at the end is the same
    // value the pointer form would have left, pair for pair, and `st.q`/`st.p` are published at
    // exactly the same pair.
    //
    // This is NOT G479's refuted `DC2_G479_QPREG`. That added two loop-carried locals to `run()`'s
    // pair loop, which G427/G442/G483 had already measured at ~1.2 IPC with 36 spills — the live
    // value cost more than the loads it deleted. This body is a 4.7 KB leaf function with its own
    // register file, entered ~160 k times/frame, and it is the ONE place the counters are provably
    // inert.
    // ---------------------------------------------------------------------------------------
    // G501 — PIN THE BLOCK'S LOOP-INVARIANT ADDRESSING PREDICATES (`G501Pin`).
    //
    // PREMISE, read off `dumpbin /DISASM` of the SHIPPED `g490BlockBody<1,0,1>` (0x3538 = 13,624
    // bytes): the leaf reloads `hb` from its STACK SLOT `[rbp+80h]` eleven times, loads the global
    // `s_g428DestStore` six times, and carries ELEVEN dead calls — `g421Desc()` ×3 and
    // `g421MaskRows()` ×8 — on arms that can never be taken. Three of the `hb` tests and one
    // `s_g428DestStore` test sit on the MAIN per-pair path (0x119 / 0x26A / 0x27B / 0x2D1), i.e.
    // on ~1.44 M pairs/frame.
    //
    // WHY THEY SURVIVED. Inside the pair loop `g436Lean` is a `constexpr` shadow (G443), so
    // `g421FastUpper` folds. The block body is a separate `noinline` leaf that takes `hb` as a
    // RUNTIME POINTER PARAMETER, so every fold G443 bought is lost at the call boundary — the same
    // class as G499's out-of-line lower dispatch, one level up.
    //
    // EXACTNESS. The block copy is selected only by `g490BlockRun = g484WideVf0 && …`, and
    // `g484WideVf0` is `g443Fixed && …`. `g443Fixed` lists **`g436Lean && g436Hb != nullptr`**
    // among its conjuncts, so `hb` is provably non-null here; G501 adds `s_g428DestStore` to that
    // same conjunction (the established convention for a promoted representation with a rollback
    // flag — `DC2_G428_NO_DEST_STORE=1` now drops to the legacy copy exactly as
    // `DC2_G436_LEGACY_ADDR=1` already did). Inside the block each pinned name therefore already
    // held the constant it is replaced by, which is G443's own argument applied to the leaf.
    //
    // This is a code-SHAPE change, so it is a separate INSTANTIATION rather than a runtime `if`
    // (G436): a selector inside the body would sit on both arms and bias the gate.
    // ---------------------------------------------------------------------------------------
    // G502 — LET A BLOCK CHAIN INTO THE NEXT ONE WITHOUT RETURNING TO run() (`G502Chain`).
    //
    // PREMISE, measured not assumed. `[G502:blk]` (this phase's census) reads **44.30% (geometry) /
    // 40.86% (resume) of blocks are immediately followed by another block** — i.e. the pair loop
    // leaves the block, walks its whole top-of-iteration envelope (pc-oob test, fused record load,
    // the pending/ebit gate, the run-length load, the budget clamp, the MIN_RUN test, the G500 tail
    // lookup, the noinline 10-argument call, the next-pc fold, the cycle update) and immediately
    // forms another block at the very next slot. At ~158 k blocks/frame that is ~67 k round trips
    // per frame that buy nothing. `DC2_G419_AB=blksplit` prices ONE such trip directly (it halves
    // every long block and adds a known number of them), so this lever is built only after its
    // ceiling is on the page — G497 §2 / G500 §8.
    //
    // WHAT THE ARM DOES. Nothing to any pair. The SAME pairs execute in the SAME order, in the same
    // blocks, with the same tails: only the return to run() between two consecutive blocks is
    // elided. `[G500:inv]` therefore has an unusually strong invariant available — `pairs/run`,
    // `cover`, `blocks` (segments), `tail1`, `tail2` and `taken` must ALL be identical across the
    // two arms, and only the new `calls=` column may differ. That is the G500 §5c control-flow
    // oracle with five extra columns.
    //
    // EXACTNESS. The chain step re-evaluates, inside the leaf, exactly the predicates run()'s hook
    // would have evaluated one iteration later, in the same order and from the same tables:
    //   * the next PC comes from the pair loop's own fold — sequential when there was no tail,
    //     and otherwise `st.pc != branchPc` for taken with the same `+8` / `+16` / wrap arithmetic.
    //   * a tail of 1 that was TAKEN leaves a pending branch target, and the pair loop is the only
    //     thing allowed to consume a delay slot, so the chain STOPS there and hands the target back.
    //     (A tail of 1 that was not taken lands on a slot whose run length is 0 by construction —
    //     that is why the tail was 1 and not 2 — so the MIN_RUN test below stops it anyway.)
    //   * `m_state.ebit` cannot have been set: every pair a block executes has `pairFlags == 0`,
    //     which is the E bit of the raw upper word, and nothing else in the leaf writes it. The
    //     hook's `!m_state.ebit` guard is therefore still true at every chain step.
    //   * `maxCycles` is an architectural suspension edge, so the remaining budget is carried and
    //     each segment is clamped to it exactly as the hook clamps the first.
    //   * `pc + 8 > codeSize` is the hook's own pc-oob test; the chain declines and lets the pair
    //     loop take its `break` with the right `stopReason`.
    // The G430 stamp base advances by the pairs each segment consumed, so every pair's stamp is the
    // one the un-chained arm would have given it.
    //
    // Arm:    DC2_G419_AB=blkchain (paired, hold 16 — thread-level code shape)
    // Kill:   DC2_G502_NO_BLKCHAIN=1
    // Stats:  DC2_G500_STAT=1 -> `[G500:inv] arm=5` with `calls=` beside `blocks=`.
    struct G502ChainOut
    {
        uint32_t consumed;        // pairs executed by the whole chain (plain + tail)
        uint32_t plain;           // plain pairs only
        uint32_t tail1;
        uint32_t tail2;
        uint32_t taken;
        uint32_t segments;        // blocks the chain absorbed (1 = no chaining happened)
        uint32_t nextPc;          // what run() must store into m_state.pc
        uint32_t pendingTarget;   // valid only when pendingValid
        bool pendingValid;
    };

    template <bool G499FastLower, bool G499QpLocal, bool G500Tail = false, bool G501Pin = false,
              bool G502Chain = false, bool G504Cursor = false, bool G506QpSeg = false>
    void g490BlockBody(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb,
                              uint32_t g500TailCount = 0u,
                              G502ChainOut *g502Out = nullptr,
                              uint32_t g502CodeSize = 0u, uint32_t g502Budget = 0u)
    {
        const G479FusedPair *const fused = s_g410Cache.fused.data();
        // G506: the segmented form needs the two counters in block locals for the same reason
        // G499's arm 2 did — the segmentation is a statement about their values across the whole
        // run, not about one pair. Both arms write back exactly once, at the single exit.
        constexpr bool G506Local = G499QpLocal || G506QpSeg;
        [[maybe_unused]] int g499Qd = 0, g499Pd = 0;
        if constexpr (G506Local)
        {
            g499Qd = *qDelay;
            g499Pd = *pDelay;
        }
        // G504: the G430 ring cursor, carried in a local for the whole block. `hb` is non-null
        // here by construction (`G501Pin` asserts it, and `g443Fixed` proves it for every caller of
        // this copy). See the exactness note at `g430PushPendingAt`.
        [[maybe_unused]] int g504Slot = 0;
#if DC2_G504_VERIFY_CUR
        [[maybe_unused]] int g504Entry = 0;
#endif
        if constexpr (G504Cursor)
        {
            g504Slot = hb->head;
#if DC2_G504_VERIFY_CUR
            g504Entry = g504Slot;
#endif
        }
        // G502: exactly ONE iteration unless `G502Chain`, in which case the `break` below is
        // replaced by the chain step. For every pre-existing instantiation this is a `for(;;)`
        // whose body ends in an unconditional `break`, which MSVC folds away entirely — the
        // shipped `<1,0,1,1>` body is verified byte-for-byte unchanged by `dumpbin /DISASM`.
        [[maybe_unused]] uint32_t g502Consumed = 0u;
        for (;;)
        {
        // G506: the segment loop. For every pre-existing instantiation `G506QpSeg` is false, so
        // `g506SegEnd` is `count`, the outer `while` runs once and MSVC folds it into the original
        // `for (j = 0; j < count; ++j)` — the shipped `<1,0,1,1,0,0>` body is unchanged.
        uint32_t j = 0u;
        for (;;)
        {
        if constexpr (G506QpSeg)
        {
            if (j >= count)
                break;
        }
        uint32_t g506SegEnd = count;
        if constexpr (G506QpSeg)
        {
            // A counter holding v at pair j publishes BEFORE pair j+v-1 (the per-pair form
            // decrements first and tests for zero, so the pair at which it reaches zero is the
            // first that sees the new value). v == 1 therefore publishes at THIS pair.
            if (g499Qd == 1)
            {
                g499Qd = 0;
                st.q = s_vuQPending;
            }
            if (g499Pd == 1)
            {
                g499Pd = 0;
                st.p = s_vuPPending;
            }
            uint32_t g506Run = count - j;
            if (g499Qd > 1 && static_cast<uint32_t>(g499Qd - 1) < g506Run)
                g506Run = static_cast<uint32_t>(g499Qd - 1);
            if (g499Pd > 1 && static_cast<uint32_t>(g499Pd - 1) < g506Run)
                g506Run = static_cast<uint32_t>(g499Pd - 1);
            // `g506Run >= 1` always: `j < count` gives at least one pair, and a counter above 1
            // contributes at least 1. Progress is therefore guaranteed and the loop terminates.
            g506SegEnd = j + g506Run;
            if (g499Qd > 1)
                g499Qd -= static_cast<int>(g506Run);
            if (g499Pd > 1)
                g499Pd -= static_cast<int>(g506Run);
        }
        for (; j < g506SegEnd; ++j)
        {
            const G479FusedPair &fp = fused[firstIndex + j];
            const uint64_t src = fp.source;
            const uint32_t lower = static_cast<uint32_t>(src);
            const uint32_t upper = static_cast<uint32_t>(src >> 32);

            if constexpr (G506QpSeg)
            {
                // Nothing: the segment boundary above already published everything this run of
                // pairs can observe, and no block-eligible pair can re-arm either counter.
            }
            else if constexpr (G499QpLocal)
            {
                if (g499Qd > 0 && --g499Qd == 0)
                    st.q = s_vuQPending;
                if (g499Pd > 0 && --g499Pd == 0)
                    st.p = s_vuPPending;
            }
            else
            {
                if (*qDelay > 0 && --(*qDelay) == 0)
                    st.q = s_vuQPending;
                if (*pDelay > 0 && --(*pDelay) == 0)
                    st.p = s_vuPPending;
            }

            // `g443Fixed` pins the four trailing arguments the pair loop passes: g428DeferPark
            // true, shadow false, g430Hist true, and the hoisted history block. Eligibility
            // guarantees `cls != 0`, so the return value is always true and the legacy
            // execUpper fallback the macro carries is unreachable here.
            if (fp.pair.upperKind != 0x6fu)
                g421FastUpper<G501Pin, G504Cursor>(st, upper, fp.pair.upperKind, true, false, true,
                                                   stampBase + static_cast<int64_t>(j), hb,
                                                   g504Slot);

            if constexpr (G499FastLower)
            {
                // G480: g422FastLower's NOP case is exactly `return;` — do not call it.
                // G481: the three block-eligible hot integer kinds run inline; no call at all.
                // (IBNE is the fourth kind the pair loop inlines and is a branch, i.e. never
                // block-eligible, so it cannot appear here.) Same tokens as the callee's cases.
                const uint8_t g499kind = fp.pair.lowerKind;
                switch (g499kind)
                {
                case G422_LK_NOP:
                    break;
                case G422_LK_IADDI:
                    G481_LOWER_BODY_IADDI(st, lower);
                    break;
                case G422_LK_IADDIU:
                    G481_LOWER_BODY_IADDIU(st, lower);
                    break;
                case G422_LK_IAND:
                    G481_LOWER_BODY_IAND(st, lower);
                    break;
                default:
                    g422FastLower(st, lower, g499kind, vuData, dataSize);
                    break;
                }
            }
            else
                g422FastLower(st, lower, fp.pair.lowerKind, vuData, dataSize);

            if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                // G484's promoted wide form, byte-for-byte with the pair loop's re-pin.
                _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                st.vi[0] = 0;
            }
        }
        // G506: for every non-segmented instantiation this is an unconditional `break` closing a
        // `for(;;)`, which MSVC folds away entirely — the same shape, and the same guarantee, the
        // G502 chain loop above already relies on.
        if constexpr (!G506QpSeg)
            break;
        }
        if constexpr (G500Tail)
        {
            // G500. `g500TailCount` is 0, 1 or 2 and the caller has already proved, from the
            // precisely invalidated sidecar, that slot `firstIndex + count` is an admissible
            // control transfer and (for 2) that slot `firstIndex + count + 1` is a plain eligible
            // pair. Statement order is the plain pair's, and the two additions are the pair loop's
            // own branch protocol — see the exactness note at the head of this file.
            if (g500TailCount != 0u)
            {
                const uint32_t g500Idx = firstIndex + count;
                const G479FusedPair &bp = fused[g500Idx];
                const uint64_t bsrc = bp.source;
                const uint32_t blower = static_cast<uint32_t>(bsrc);
                const uint32_t bupper = static_cast<uint32_t>(bsrc >> 32);

                // G506: the tail is at most two pairs, so it keeps the per-pair form — segmenting
                // it would buy nothing and the branch pair is the one place `st.pc` is written.
                // It reads the SEGMENTED arm's locals, which the loop above left exact.
                if constexpr (G506Local)
                {
                    if (g499Qd > 0 && --g499Qd == 0)
                        st.q = s_vuQPending;
                    if (g499Pd > 0 && --g499Pd == 0)
                        st.p = s_vuPPending;
                }
                else
                {
                    if (*qDelay > 0 && --(*qDelay) == 0)
                        st.q = s_vuQPending;
                    if (*pDelay > 0 && --(*pDelay) == 0)
                        st.p = s_vuPPending;
                }

                if (bp.pair.upperKind != 0x6fu)
                    g421FastUpper<G501Pin, G504Cursor>(st, bupper, bp.pair.upperKind, true, false,
                                                       true,
                                                       stampBase + static_cast<int64_t>(count), hb,
                                                       g504Slot);

                // The architectural PC of THIS pair, which every branch body reads (and BAL/JALR
                // write back as `(st.pc + 16) / 8`). One store per block, not per pair.
                st.pc = g500Idx * 8u;
                // IBNE is 4.41% of all pairs — half of every control transfer on this route — and
                // the pair loop has run it inline since G481. Same tokens as g422FastLower's own
                // case (vu1_g481_hot_lower.inc), so the executed statements are identical.
                // (The G481 bodies are brace blocks, not do/while macros — hence the explicit
                // braces on both arms.)
                if (bp.pair.lowerKind == G422_LK_IBNE)
                {
                    G481_LOWER_BODY_IBNE(st, blower);
                }
                else
                {
                    g422FastLower(st, blower, bp.pair.lowerKind, vuData, dataSize);
                }

                if ((bp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                {
                    _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                    st.vi[0] = 0;
                }

                if (g500TailCount == 2u)
                {
                    // The DELAY SLOT. A plain eligible pair by construction; nothing it can
                    // execute reads `st.pc`, so the branch's write above is invisible to it and
                    // the caller still finds the branch outcome there.
                    const G479FusedPair &dp = fused[g500Idx + 1u];
                    const uint64_t dsrc = dp.source;
                    const uint32_t dlower = static_cast<uint32_t>(dsrc);
                    const uint32_t dupper = static_cast<uint32_t>(dsrc >> 32);

                    if constexpr (G506Local)
                    {
                        if (g499Qd > 0 && --g499Qd == 0)
                            st.q = s_vuQPending;
                        if (g499Pd > 0 && --g499Pd == 0)
                            st.p = s_vuPPending;
                    }
                    else
                    {
                        if (*qDelay > 0 && --(*qDelay) == 0)
                            st.q = s_vuQPending;
                        if (*pDelay > 0 && --(*pDelay) == 0)
                            st.p = s_vuPPending;
                    }

                    if (dp.pair.upperKind != 0x6fu)
                        g421FastUpper<G501Pin, G504Cursor>(
                            st, dupper, dp.pair.upperKind, true, false, true,
                            stampBase + static_cast<int64_t>(count) + 1, hb, g504Slot);

                    if constexpr (G499FastLower)
                    {
                        const uint8_t g500kind = dp.pair.lowerKind;
                        switch (g500kind)
                        {
                        case G422_LK_NOP:
                            break;
                        case G422_LK_IADDI:
                            G481_LOWER_BODY_IADDI(st, dlower);
                            break;
                        case G422_LK_IADDIU:
                            G481_LOWER_BODY_IADDIU(st, dlower);
                            break;
                        case G422_LK_IAND:
                            G481_LOWER_BODY_IAND(st, dlower);
                            break;
                        default:
                            g422FastLower(st, dlower, g500kind, vuData, dataSize);
                            break;
                        }
                    }
                    else
                        g422FastLower(st, dlower, dp.pair.lowerKind, vuData, dataSize);

                    if ((dp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                    {
                        _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                        st.vi[0] = 0;
                    }
                }
            }
        }
        if constexpr (!G502Chain)
            break;
        else
        {
            // ------------------------------------------------------------------------------
            // G502 CHAIN STEP. Everything here is the pair loop's own next-iteration logic,
            // evaluated once per SEGMENT instead of once per block round trip. See the exactness
            // note at the head of this template.
            // ------------------------------------------------------------------------------
            g502Out->segments += 1u;
            g502Out->plain += count;
            if (g500TailCount == 1u)
                ++g502Out->tail1;
            else if (g500TailCount == 2u)
                ++g502Out->tail2;
            g502Consumed += count + g500TailCount;

            // The pair loop's own next-pc fold, with the same taken test and the same wrap.
            uint32_t g502Next;
            bool g502Pending = false;
            uint32_t g502Target = 0u;
            if (g500TailCount == 0u)
                g502Next = (firstIndex + count) * 8u;
            else
            {
                const uint32_t g502BPc = (firstIndex + count) * 8u;
                const bool g502Taken = (st.pc != g502BPc);
                if (g502Taken)
                    ++g502Out->taken;
                if (g500TailCount == 2u)
                    g502Next = g502Taken ? (st.pc + 8u) : (g502BPc + 16u);
                else
                {
                    if (g502Taken)
                    {
                        g502Target = st.pc + 8u;
                        if (g502Target >= g502CodeSize)
                            g502Target = 0u;
                        g502Pending = true;
                    }
                    g502Next = g502BPc + 8u;
                }
            }
            if (g502Next >= g502CodeSize)
                g502Next = 0u;

            // Can this chain continue? Exactly run()'s hook, one iteration later.
            if (!g502Pending && g502Consumed < g502Budget && g502Next + 8u <= g502CodeSize)
            {
                const uint32_t g502Rem = g502Budget - g502Consumed;
                const uint32_t g502Avail =
                    static_cast<uint32_t>(s_g410Cache.g490RunLen[g502Next >> 3u]);
                const uint32_t g502Count = g502Avail < g502Rem ? g502Avail : g502Rem;
                if (g502Count >= G490_MIN_RUN)
                {
                    uint32_t g502T = 0u;
                    if (g502Count == g502Avail)
                    {
                        const uint32_t g502TPc = g502Next + 8u * g502Avail;
                        if (g502TPc + 8u <= g502CodeSize)
                        {
                            g502T = static_cast<uint32_t>(
                                s_g410Cache.g500Tail[g502TPc >> 3u]);
                            if (g502T == 2u && g502TPc + 16u > g502CodeSize)
                                g502T = 1u;
                            if (g502Avail + g502T > g502Rem)
                                g502T = 0u;
                        }
                    }
                    stampBase += static_cast<int64_t>(count) +
                                 static_cast<int64_t>(g500TailCount);
                    firstIndex = g502Next >> 3u;
                    count = g502Count;
                    g500TailCount = g502T;
                    continue;
                }
            }
            g502Out->consumed = g502Consumed;
            g502Out->nextPc = g502Next;
            g502Out->pendingValid = g502Pending;
            g502Out->pendingTarget = g502Target;
            break;
        }
        }
        if constexpr (G506Local)
        {
            // Exactly the values the pointer form would have left: neither counter can be
            // RE-ARMED inside a block (see the eligibility argument above), so a single write-back
            // is equivalent to the per-pair stores, and every consumer of `*qDelay`/`*pDelay`
            // lives outside this call.
            *qDelay = g499Qd;
            *pDelay = g499Pd;
        }
        // G504: the ring cursor's single write-back. This is the ONLY exit of the function — the
        // `for(;;)` above is either folded away entirely (every non-chain instantiation ends its
        // body in an unconditional `break`) or left through the chain step's `break`, and there is
        // no `return` anywhere in the body. Every reader of `H.head` outside this call
        // (`g430PushPending` in the pair loop, `g430PushEager`, run()'s prologue) therefore sees
        // exactly the cursor the per-push form would have left.
        if constexpr (G504Cursor)
        {
            // ORACLE (DC2_G504_VERIFY=1, default off). The whole lever rests on one claim: NOTHING
            // else writes `H.head` while a block runs. That claim is checkable directly — if it
            // holds, `hb->head` is still the value read at entry. This catches the failure the
            // structural argument could get wrong (a lower kind that reaches the history, a future
            // caller that pushes from inside the leaf) far more sharply than any output compare.
            //
            // It cannot be checked with `DC2_G430_VERIFY=1` instead: that flag is a conjunct of
            // `g443Fixed` (vu1_upper_opcodes.inc), so arming it drops the whole run onto a loop copy
            // that HAS NO BLOCK HOOK — the G500 §2 / G501 §9 "the census flag selects a different
            // loop copy" trap. One branch per BLOCK (~158 k/frame, never taken), inside a leaf whose
            // entry already costs 1.89 ns, so the gate is not measurably biased by it — and what
            // bias there is runs AGAINST the candidate, since only this arm carries the test.
#if DC2_G504_VERIFY_CUR
            if (g504VerifyOn() && hb->head != g504Entry)
            {
                static std::atomic<uint32_t> s_bad{0};
                const uint32_t n = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u)
                    std::fprintf(stderr,
                                 "[G504:verify-BAD] head moved under a block: entry=%d now=%d "
                                 "local=%d firstIndex=%u count=%u n=%u\n",
                                 g504Entry, hb->head, g504Slot, firstIndex, count, n);
            }
#endif
            hb->head = g504Slot;
        }
    }

    // The three instantiations. `noinline` for the reason recorded above the body: a second
    // inlined copy of the SIMD upper + fast lower inside run() would be paid by every pair to help
    // a subset. `<true,true>` is the shipped default; `<true,false>` is the `qpblock` gate's
    // control arm; `<false,false>` is the pre-G499 body and the `DC2_G499_NO_BLKLOWER` rollback.
    // The fourth combination `<false,true>` is deliberately NOT instantiated — no gate needs it,
    // and a copy nothing selects is text nothing pays for (G490).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g490ExecuteBlock(VU1State &st, uint32_t firstIndex, uint32_t count,
                          uint8_t *vuData, uint32_t dataSize,
                          int *qDelay, int *pDelay,
                          int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<false, false>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                    stampBase, hb);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g499ExecuteBlockFast(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<true, false>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                   stampBase, hb);
    }

#if DC2_G500_BLKBRANCH
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g500ExecuteBlockTail(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        // G501: `G501Pin` is UNCONDITIONAL here — see the note at `g490BlockBody`. It is not an
        // arm: it deletes code `g443Fixed` proves unreachable, so there is nothing for a control
        // arm to be different about, and the pinned instantiation is strictly smaller
        // (0x3364 vs 0x3538) with eleven fewer calls. The rollback is the one that already
        // existed — `DC2_G436_LEGACY_ADDR=1` or `DC2_G428_NO_DEST_STORE=1` falsifies `g443Fixed`
        // and drops the whole run onto the legacy loop copy, which has no block hook at all.
        g490BlockBody<true, false, true, true>(st, firstIndex, count, vuData, dataSize, qDelay,
                                               pDelay, stampBase, hb, tailCount);
    }

#if DC2_G504_HISTCUR
    // G504: arm 3's body with the G430 ring cursor in a block local. A separate INSTANTIATION rather
    // than a runtime `if` for the same reason as every other arm in this file (G436): a selector
    // inside the leaf would sit on BOTH arms and bias the gate. The wrapper is `noinline` so run()'s
    // own text is byte-identical between the two arms — only which leaf is called differs.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g504ExecuteBlockTailCur(VU1State &st, uint32_t firstIndex, uint32_t count,
                                 uint8_t *vuData, uint32_t dataSize,
                                 int *qDelay, int *pDelay,
                                 int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, true>(st, firstIndex, count, vuData, dataSize,
                                                            qDelay, pDelay, stampBase, hb,
                                                            tailCount);
    }
#endif // DC2_G504_HISTCUR

#if DC2_G506_QPSEG
    // G506: arm 3's body with the Q/P delay advance hoisted out of the per-pair loop by
    // segmentation. A separate INSTANTIATION rather than a runtime `if` for the same reason as every
    // other arm in this file (G436): a selector inside the leaf would sit on BOTH arms and bias the
    // gate. The wrapper is `noinline` so run()'s own text is byte-identical between the two arms —
    // only which leaf is called differs.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g506ExecuteBlockTailQpSeg(VU1State &st, uint32_t firstIndex, uint32_t count,
                                   uint8_t *vuData, uint32_t dataSize,
                                   int *qDelay, int *pDelay,
                                   int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }
#endif // DC2_G506_QPSEG

#if DC2_G502_BLKCHAIN
    // G502: the same body with the chain step compiled in. A separate INSTANTIATION rather than a
    // runtime `if` (G436) — a selector inside the leaf would sit on both arms and bias the gate —
    // and a separate `noinline` wrapper so run()'s own text is unchanged (G481's inline-budget
    // cliff binds on run(), not on a leaf).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g502ExecuteBlockChain(VU1State &st, uint32_t firstIndex, uint32_t count,
                               uint8_t *vuData, uint32_t dataSize,
                               int *qDelay, int *pDelay,
                               int64_t stampBase, G430HistBlock *hb, uint32_t tailCount,
                               G502ChainOut &out, uint32_t codeSize, uint32_t budget)
    {
        g490BlockBody<true, false, true, true, true>(st, firstIndex, count, vuData, dataSize,
                                                     qDelay, pDelay, stampBase, hb, tailCount,
                                                     &out, codeSize, budget);
    }
#endif // DC2_G502_BLKCHAIN
#endif // DC2_G500_BLKBRANCH

#if DC2_G499_QPLOCAL
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g499ExecuteBlockFastQp(VU1State &st, uint32_t firstIndex, uint32_t count,
                                uint8_t *vuData, uint32_t dataSize,
                                int *qDelay, int *pDelay,
                                int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<true, true>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                  stampBase, hb);
    }
#endif // DC2_G499_QPLOCAL

    // One-shot proof of selection (G457): a copy that was never selected proves nothing, and a
    // run-length table that covers nothing is a null with no information in it.
    void g490ReportArmed(bool armed)
    {
        static const bool s_show = g490StatOn() || (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_show)
            return;
        static std::atomic<uint32_t> s_once[2] = {};
        const int slot = armed ? 1 : 0;
        if (s_once[slot].fetch_add(1u, std::memory_order_relaxed) != 0u)
            return;
        uint32_t eligible = 0u;
        uint32_t runs = 0u;
        for (size_t i = 0u; i < s_g410Cache.g490RunLen.size(); ++i)
        {
            if (s_g410Cache.g490RunLen[i] != 0u)
                ++eligible;
            if (s_g410Cache.g490RunLen[i] >= G490_MIN_RUN &&
                (i == 0u || s_g410Cache.g490RunLen[i - 1u] == 0u))
                ++runs;
        }
        std::fprintf(stderr,
                     "[G490:armed] block=%d eligibleSlots=%u (%.2f%%) runStarts=%u\n",
                     armed ? 1 : 0, eligible,
                     100.0 * static_cast<double>(eligible) /
                         static_cast<double>(s_g410Cache.g490RunLen.size()),
                     runs);
        std::fflush(stderr);
    }

    // G499: the same one-shot proof of selection, per arm (G457). A paired A/B in which only one
    // arm was ever built measures nothing, and this phase's whole premise is that a copy can be
    // present in the source and unreachable on the shipped path (G490's own lesson).
    void g499ReportArmed(int arm)
    {
        static const bool s_show = g499StatOn() || (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_show)
            return;
        // G501: five slots, not three. With `s_once[3]` and `arm > 2 -> slot 0`, the `blkpin`
        // gate's two arms (3 and 4) BOTH folded onto slot 0, so exactly one `[G499:blk]` line
        // would print and the run could not prove both bodies were built — the G457 failure this
        // helper exists to prevent. (G500's arms happened to be 1 and 3, which landed on different
        // slots by luck.)
        static std::atomic<uint32_t> s_once[8] = {};
        const int slot = (arm < 0 || arm > 7) ? 0 : arm;
        if (s_once[slot].fetch_add(1u, std::memory_order_relaxed) != 0u)
            return;
        std::fprintf(stderr, "[G499:blk] arm=%d\n", arm);
        std::fflush(stderr);
    }
} // namespace
